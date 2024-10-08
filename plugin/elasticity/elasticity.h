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

#ifndef MUJOCO_PLUGIN_ELASTICITY_ELASTICITY_H_
#define MUJOCO_PLUGIN_ELASTICITY_ELASTICITY_H_

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <mujoco/mujoco.h>

namespace mujoco::plugin::elasticity {

struct PairHash
{
    template <class T1, class T2>
    std::size_t operator() (const std::pair<T1, T2>& pair) const {
        return std::hash<T1>()(pair.first) ^ std::hash<T2>()(pair.second);
    }
};

struct Stencil2D {
  static constexpr int kNumEdges = 3;
  static constexpr int kNumVerts = 3;
  static constexpr int edge[kNumEdges][2] = {{1, 2}, {2, 0}, {0, 1}};
  int vertices[kNumVerts];
  int edges[kNumEdges];
};

struct Stencil3D {
  static constexpr int kNumEdges = 6;
  static constexpr int kNumVerts = 4;
  static constexpr int edge[kNumEdges][2] = {{0, 1}, {1, 2}, {2, 0},
                                             {2, 3}, {0, 3}, {1, 3}};
  int vertices[kNumVerts];
  int edges[kNumEdges];
};

// gradients of edge lengths with respect to vertex positions
template <typename T>
void inline GradSquaredLengths(mjtNum gradient[T::kNumEdges][2][3],
                               const mjtNum* x,
                               const int v[T::kNumVerts]) {
  for (int e = 0; e < T::kNumEdges; e++) {
    for (int d = 0; d < 3; d++) {
      gradient[e][0][d] = x[3*v[T::edge[e][0]]+d] - x[3*v[T::edge[e][1]]+d];
      gradient[e][1][d] = x[3*v[T::edge[e][1]]+d] - x[3*v[T::edge[e][0]]+d];
    }
  }
}

template <typename T>
inline void ComputeForce(std::vector<mjtNum>& qfrc_passive,
                         const std::vector<mjtNum>& elongationglob,
                         const mjModel* m, int flex,
                         const mjtNum* xpos) {
  mju_zero(qfrc_passive.data(), qfrc_passive.size());
  mjtNum* k = m->flex_stiffness + 21 * m->flex_elemadr[flex];

  int dim = m->flex_dim[flex];
  const int* elem = m->flex_elem + m->flex_elemdataadr[flex];
  const int* edgeelem = m->flex_elemedge + m->flex_elemedgeadr[flex];

  // compute force element-by-element
  for (int t = 0; t < m->flex_elemnum[flex]; t++)  {
    const int* v = elem + (dim+1) * t;

    // compute length gradient with respect to dofs
    mjtNum gradient[T::kNumEdges][2][3];
    GradSquaredLengths<T>(gradient, xpos, v);

    // extract elongation of edges belonging to this element
    mjtNum elongation[T::kNumEdges];
    for (int e = 0; e < T::kNumEdges; e++) {
      int idx = edgeelem[t * T::kNumEdges + e];
      elongation[e] = elongationglob[idx];
    }

    // unpack triangular representation
    mjtNum metric[T::kNumEdges*T::kNumEdges];

    int id = 0;
    for (int ed1 = 0; ed1 < T::kNumEdges; ed1++) {
      for (int ed2 = ed1; ed2 < T::kNumEdges; ed2++) {
        metric[T::kNumEdges*ed1 + ed2] = k[21*t + id];
        metric[T::kNumEdges*ed2 + ed1] = k[21*t + id++];
      }
    }

    // we now multiply the elongations by the precomputed metric tensor,
    // notice that if metric=diag(1/reference) then this would yield a
    // mass-spring model

    // compute local force
    mjtNum force[T::kNumVerts*3] = {0};
    for (int ed1 = 0; ed1 < T::kNumEdges; ed1++) {
      for (int ed2 = 0; ed2 < T::kNumEdges; ed2++) {
        for (int i = 0; i < 2; i++) {
          for (int x = 0; x < 3; x++) {
            force[3 * T::edge[ed2][i] + x] -=
                elongation[ed1] * gradient[ed2][i][x] *
                metric[T::kNumEdges * ed1 + ed2];
          }
        }
      }
    }

    // insert into global force
    for (int i = 0; i < T::kNumVerts; i++) {
      for (int x = 0; x < 3; x++) {
        qfrc_passive[3*v[i]+x] += force[3*i+x];
      }
    }
  }
}

// add flex force to degrees of freedom
inline void AddFlexForce(mjtNum* qfrc,
                         const std::vector<mjtNum>& force,
                         const mjModel* m, mjData* d,
                         const mjtNum* xpos,
                         int f0) {
  int* bodyid = m->flex_vertbodyid + m->flex_vertadr[f0];

  for (int v = 0; v < m->flex_vertnum[f0]; v++) {
    int bid = bodyid[v];
    if (m->body_simple[bid] != 2) {
      // this should only occur for pinned flex vertices
      mj_applyFT(m, d, force.data() + 3*v, 0, xpos + 3*v, bid, qfrc);
    } else {
      int body_dofnum = m->body_dofnum[bid];
      int body_dofadr = m->body_dofadr[bid];
      for (int x = 0; x < body_dofnum; x++) {
        qfrc[body_dofadr+x] += force[3*v+x];
      }
    }
  }
}

// compute metric tensor of edge lengths inner product
template <typename T>
void inline MetricTensor(mjtNum* metric, int idx, mjtNum mu,
                         mjtNum la, const mjtNum basis[T::kNumEdges][9]) {
  mjtNum trE[T::kNumEdges] = {0};
  mjtNum trEE[T::kNumEdges*T::kNumEdges] = {0};
  mjtNum k[T::kNumEdges*T::kNumEdges];

  // compute first invariant i.e. trace(strain)
  for (int e = 0; e < T::kNumEdges; e++) {
    for (int i = 0; i < 3; i++) {
      trE[e] += basis[e][4*i];
    }
  }

  // compute second invariant i.e. trace(strain^2)
  for (int ed1 = 0; ed1 < T::kNumEdges; ed1++) {
    for (int ed2 = 0; ed2 < T::kNumEdges; ed2++) {
      for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
          trEE[T::kNumEdges*ed1+ed2] += basis[ed1][3*i+j] * basis[ed2][3*j+i];
        }
      }
    }
  }

  // assembly of strain metric tensor
  for (int ed1 = 0; ed1 < T::kNumEdges; ed1++) {
    for (int ed2 = 0; ed2 < T::kNumEdges; ed2++) {
      k[T::kNumEdges*ed1 + ed2] = mu * trEE[T::kNumEdges * ed1 + ed2] +
                                  la * trE[ed2] * trE[ed1];
    }
  }

  // copy to triangular representation
  int id = 0;
  for (int ed1 = 0; ed1 < T::kNumEdges; ed1++) {
    for (int ed2 = ed1; ed2 < T::kNumEdges; ed2++) {
      metric[21*idx + id++] = k[T::kNumEdges*ed1 + ed2];
    }
  }

  if (id != T::kNumEdges*(T::kNumEdges+1)/2) {
    mju_error("incorrect stiffness matrix size");
  }
}

// convert from Flex connectivity to stencils
template <typename T>
int CreateStencils(std::vector<T>& elements,
                   std::vector<std::pair<int, int>>& edges,
                   const std::vector<int>& simplex,
                   const std::vector<int>& edgeidx);

// copied from mjXUtil
void String2Vector(const std::string& txt, std::vector<int>& vec);

// reads numeric attributes
bool CheckAttr(const char* name, const mjModel* m, int instance);

}  // namespace mujoco::plugin::elasticity

#endif  // MUJOCO_PLUGIN_ELASTICITY_ELASTICITY_H_
