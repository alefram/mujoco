// Copyright 2022 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include <mujoco/mjplugin.h>
#include <mujoco/mjtnum.h>
#include <mujoco/mujoco.h>
#include "elasticity.h"
#include "solid.h"


namespace mujoco::plugin::elasticity {
namespace {

// local tetrahedron numbering
constexpr int kNumEdges = Stencil3D::kNumEdges;
constexpr int kNumVerts = Stencil3D::kNumVerts;
constexpr int face[kNumVerts][3] = {{2, 1, 0}, {0, 1, 3}, {1, 2, 3}, {2, 0, 3}};
constexpr int e2f[kNumEdges][2] = {{2, 3}, {1, 3}, {2, 1},
                                   {1, 0}, {0, 2}, {0, 3}};

// volume of a tetrahedron
mjtNum ComputeVolume(const mjtNum* x, const int v[kNumVerts]) {
  mjtNum normal[3];
  mjtNum edge1[3];
  mjtNum edge2[3];
  mjtNum edge3[3];

  mju_sub3(edge1, x+3*v[1], x+3*v[0]);
  mju_sub3(edge2, x+3*v[2], x+3*v[0]);
  mju_sub3(edge3, x+3*v[3], x+3*v[0]);
  mju_cross(normal, edge2, edge1);

  return mju_dot3(normal, edge3) / 6;
}

// compute local basis
void ComputeBasis(mjtNum basis[9], const mjtNum* x, const int v[kNumVerts],
                  const int faceL[3], const int faceR[3], mjtNum volume) {
  mjtNum normalL[3], normalR[3];
  mjtNum edgesL[6], edgesR[6];

  mju_sub3(edgesL+0, x+3*v[faceL[1]], x+3*v[faceL[0]]);
  mju_sub3(edgesL+3, x+3*v[faceL[2]], x+3*v[faceL[0]]);
  mju_sub3(edgesR+0, x+3*v[faceR[1]], x+3*v[faceR[0]]);
  mju_sub3(edgesR+3, x+3*v[faceR[2]], x+3*v[faceR[0]]);

  mju_cross(normalL, edgesL, edgesL+3);
  mju_cross(normalR, edgesR, edgesR+3);

  // we use as basis the symmetrized tensor products of the area normals of the
  // two faces not adjacent to the edge; this is the 3D equivalent to the basis
  // proposed in Weischedel "A discrete geometric view on shear-deformable shell
  // models" in the remark at the end of section 4.1. This is also equivalent to
  // linear finite elements but in a coordinate-free formulation.

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      basis[3*i+j] = ( normalL[i]*normalR[j] +
                       normalR[i]*normalL[j] ) / (36*2*volume*volume);
    }
  }
}

}  // namespace

// factory function
std::optional<Solid> Solid::Create(const mjModel* m, mjData* d, int instance) {
    if (CheckAttr("face", m, instance) &&
        CheckAttr("edge", m, instance) &&
        CheckAttr("poisson", m, instance) &&
        CheckAttr("young", m, instance)) {
        mjtNum nu = strtod(mj_getPluginConfig(m, instance, "poisson"), nullptr);
        mjtNum E = strtod(mj_getPluginConfig(m, instance, "young"), nullptr);
        mjtNum damp =
            strtod(mj_getPluginConfig(m, instance, "damping"), nullptr);
        return Solid(m, d, instance, nu, E, damp);
    } else {
        mju_warning("Invalid parameter specification in solid plugin");
        return std::nullopt;
    }
}

// plugin constructor
Solid::Solid(const mjModel* m, mjData* d, int instance, mjtNum nu, mjtNum E,
             mjtNum damp)
    : f0(-1), damping(damp) {
  // count plugin bodies
  nv = ne = 0;
  for (int i = 1; i < m->nbody; i++) {
    if (m->body_plugin[i] == instance) {
      if (!nv++) {
        i0 = i;
      }
    }
  }

  // count flexes
  for (int i = 0; i < m->nflex; i++) {
    for (int j = 0; j < m->flex_vertnum[i]; j++) {
      if (m->flex_vertbodyid[m->flex_vertadr[i]+j] == i0) {
        f0 = i;
        nv = m->flex_vertnum[f0];
        if (m->flex_dim[i] != 3) {  // SHOULD NOT OCCUR
          mju_error("mujoco.elasticity.solid requires a 3D mesh");
        }
      }
    }
  }

  // vertex positions
  mjtNum* body_pos = m->flex_xvert0 + 3*m->flex_vertadr[f0];

  // loop over all tetrahedra
  const int* elem = m->flex_elem + m->flex_elemdataadr[f0];
  for (int t = 0; t < m->flex_elemnum[f0]; t++) {
    const int* v = elem + (m->flex_dim[f0]+1) * t;
    for (int i = 0; i < kNumVerts; i++) {
      int bi = m->flex_vertbodyid[m->flex_vertadr[f0]+v[i]];
      if (bi && m->body_plugin[bi] != instance) {
        mju_error("Body %d does not have plugin instance %d", bi, instance);
      }
    }

    // tetrahedron volume
    mjtNum volume = ComputeVolume(body_pos, v);

    // local geometric quantities
    mjtNum basis[kNumEdges][9] = {{0}, {0}, {0}, {0}, {0}, {0}};

    // compute edge basis
    for (int e = 0; e < kNumEdges; e++) {
      ComputeBasis(basis[e], body_pos, v,
                   face[e2f[e][0]], face[e2f[e][1]], volume);
    }

    // material parameters
    mjtNum mu = E / (2*(1+nu)) * volume;
    mjtNum la = E*nu / ((1+nu)*(1-2*nu)) * volume;

    // compute metric tensor
    // TODO: do not write in a const mjModel
    MetricTensor<Stencil3D>(m->flex_stiffness + 21 * m->flex_elemadr[f0], t, mu,
                            la, basis);
  }

  // allocate array
  ne = m->flex_edgenum[f0];
  elongation.assign(ne, 0);
  force.assign(3*nv, 0);
}

void Solid::Compute(const mjModel* m, mjData* d, int instance) {
  mjtNum kD = damping / m->opt.timestep;

  // read edge lengths
  mjtNum* deformed = d->flexedge_length + m->flex_edgeadr[f0];
  mjtNum* ref = m->flexedge_length0 + m->flex_edgeadr[f0];

  // m->flexedge_length0 is not initialized when the plugin is constructed
  if (prev.empty()) {
    prev.assign(ne, 0);
    memcpy(prev.data(), ref, sizeof(mjtNum) * ne);
  }

  // we add generalized Rayleigh damping as decribed in Section 5.2 of
  // Kharevych et al., "Geometric, Variational Integrators for Computer
  // Animation" http://multires.caltech.edu/pubs/DiscreteLagrangian.pdf

  for (int idx = 0; idx < ne; idx++) {
    elongation[idx] = deformed[idx]*deformed[idx] - ref[idx]*ref[idx] +
                    ( deformed[idx]*deformed[idx] - prev[idx]*prev[idx] ) * kD;
  }

  // compute gradient of elastic energy and insert into passive force
  int flex_vertadr = m->flex_vertadr[f0];
  mjtNum* xpos = d->flexvert_xpos + 3*flex_vertadr;
  mjtNum* qfrc = d->qfrc_passive;

  ComputeForce<Stencil3D>(force, elongation, m, f0, xpos);

  // insert into passive force
  AddFlexForce(qfrc, force, m, d, xpos, f0);

  // update stored lengths
  if (kD > 0) {
    memcpy(prev.data(), deformed, sizeof(mjtNum) * ne);
  }
}



void Solid::RegisterPlugin() {
  mjpPlugin plugin;
  mjp_defaultPlugin(&plugin);

  plugin.name = "mujoco.elasticity.solid";
  plugin.capabilityflags |= mjPLUGIN_PASSIVE;

  const char* attributes[] = {"face", "edge", "young", "poisson", "damping", "thickness"};
  plugin.nattribute = sizeof(attributes) / sizeof(attributes[0]);
  plugin.attributes = attributes;
  plugin.nstate = +[](const mjModel* m, int instance) { return 0; };

  plugin.init = +[](const mjModel* m, mjData* d, int instance) {
    auto elasticity_or_null = Solid::Create(m, d, instance);
    if (!elasticity_or_null.has_value()) {
      return -1;
    }
    d->plugin_data[instance] = reinterpret_cast<uintptr_t>(
        new Solid(std::move(*elasticity_or_null)));
    return 0;
  };
  plugin.destroy = +[](mjData* d, int instance) {
    delete reinterpret_cast<Solid*>(d->plugin_data[instance]);
    d->plugin_data[instance] = 0;
  };
  plugin.compute =
      +[](const mjModel* m, mjData* d, int instance, int capability_bit) {
        auto* elasticity = reinterpret_cast<Solid*>(d->plugin_data[instance]);
        elasticity->Compute(m, d, instance);
      };

  mjp_registerPlugin(&plugin);
}

}  // namespace mujoco::plugin::elasticity
