// Phase 5D — collision query/apply path + floor probe.
// Faithful transcription of the original algorithms (see header for the
// evidence map). Functions are named/ordered after the originals.

#include "core/collision_query.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mdk {
namespace {

// ---------------------------------------------------------------------------
// FUN_0045ce58 — 6-float AABB overlap test
// ---------------------------------------------------------------------------

int aabbOverlap(const float* a, const float* b) {
  return !(a[3] < b[0] || b[3] < a[0]) && !(a[4] < b[1] || b[4] < a[1]) &&
         !(a[5] < b[2] || b[5] < a[2]);
}

// ---------------------------------------------------------------------------
// FUN_0045c838 — segment vs AABB, 2.5D resolver
//   start/end: the moving segment; box: Minkowski-expanded element AABB
//   outClamp: earliest face-crossing point; outAlt: face-clamped target
//   Returns 0 = no interaction, 1 = face clamp, 2 = inside/no crossing.
// ---------------------------------------------------------------------------

int segAabbResolve(const float* start, const float* target, const float* box,
                   float* outClamp, float* outAlt) {
  const float dx = target[0] - start[0];
  const float dy = target[1] - start[1];
  const float dz = target[2] - start[2];
  std::uint32_t hitMask = 0;
  float bestT = 1.1f;
  if (start[2] < box[2] && target[2] < box[2]) return 0;
  if (box[5] < start[2] && box[5] < target[2]) return 0;
  if (box[0] < start[0]) {
    if (box[3] <= start[0]) {
      hitMask = 2;
      if (target[0] < box[3]) {
        const float t = (box[3] - start[0]) / dx;
        if (t < 1.1f) {
          const float y = t * dy + start[1];
          if (y <= box[4] && box[1] <= y) {
            outClamp[0] = box[3];
            outClamp[1] = y;
            outClamp[2] = t * dz + start[2];
            bestT = t;
            if (outAlt) {
              outAlt[0] = box[3];
              outAlt[1] = target[1];
              outAlt[2] = target[2];
            }
          }
        }
      }
    }
  } else {
    hitMask = 1;
    if (box[0] < target[0]) {
      const float t = (box[0] - start[0]) / dx;
      if (t < 1.1f) {
        const float y = t * dy + start[1];
        if (y <= box[4] && box[1] <= y) {
          outClamp[0] = box[0];
          outClamp[1] = y;
          outClamp[2] = t * dz + start[2];
          bestT = t;
          if (outAlt) {
            outAlt[0] = box[0];
            outAlt[1] = target[1];
            outAlt[2] = target[2];
          }
        }
      }
    }
  }
  if (box[1] < start[1]) {
    if (start[1] < box[4] || (hitMask |= 8, box[4] <= target[1])) {
      // no high-y crossing
    } else {
      const float t = (box[4] - start[1]) / dy;
      if (bestT <= t) {
        // later crossing — keep the earlier one
      } else {
        const float x = t * dx + start[0];
        if (box[3] < x || x < box[0]) {
          // off the face
        } else {
          outClamp[0] = x;
          outClamp[1] = box[4];
          outClamp[2] = t * dz + start[2];
          bestT = t;
          if (outAlt) {
            outAlt[0] = target[0];
            outAlt[1] = box[4];
            outAlt[2] = target[2];
          }
        }
      }
    }
  } else {
    hitMask |= 4;
    if (target[1] <= box[1]) {
      // no low-y crossing
    } else {
      const float t = (box[1] - start[1]) / dy;
      if (bestT <= t) {
        // later crossing — keep the earlier one
      } else {
        const float x = t * dx + start[0];
        if (box[3] < x || x < box[0]) {
          // off the face
        } else {
          outClamp[0] = x;
          outClamp[1] = box[1];
          outClamp[2] = t * dz + start[2];
          bestT = t;
          if (outAlt) {
            outAlt[0] = target[0];
            outAlt[1] = box[1];
            outAlt[2] = target[2];
          }
        }
      }
    }
  }
  if (hitMask != 0) {
    if (bestT > 1.0f) return 0;
    return 1;
  }
  outClamp[0] = start[0];
  outClamp[1] = start[1];
  outClamp[2] = start[2];
  if (outAlt) {
    const float cx = (box[0] + box[3]) * 0.5f;
    const float cy = (box[1] + box[4]) * 0.5f;
    outAlt[0] = ((start[0] <= cx && dx < 0.0f) || (cx <= start[0] && dx > 0.0f))
                    ? target[0]
                    : start[0];
    if ((cy < start[1] || dy >= 0.0f) && (start[1] < cy || dy <= 0.0f)) {
      outAlt[1] = start[1];
      outAlt[2] = target[2];
      return 2;
    }
    outAlt[1] = target[1];
    outAlt[2] = target[2];
  }
  return 2;
}

// ---------------------------------------------------------------------------
// FUN_004089c0 — projected box-vs-triangle test
//   contact: the sweep contact point (box centre)
//   gate:    per-axis test-enable vector (the original passes extVec)
//   ext:     box half-extents
//   Returns 1 = overlap on every axis projection, 0 = separated.
// ---------------------------------------------------------------------------

int boxTri(const float* contact, const float* ext, const float* gate,
           const float* v0, const float* v1, const float* v2) {
  static const int kAxis[3][2] = {{1, 2}, {0, 2}, {0, 1}}; // DAT_00499f54
  const float* verts[3] = {v0, v1, v2};
  for (int a = 0; a < 3; ++a) {
    if (std::fabs(gate[a]) < 0.1f) continue; // DAT_004945b8
    const int a0 = kAxis[a][0], a1 = kAxis[a][1];
    float rel[3][2];
    std::uint32_t codes[3] = {0, 0, 0};
    int outside = 0;
    for (int i = 0; i < 3; ++i) {
      rel[i][0] = verts[i][a0] - contact[a0];
      rel[i][1] = verts[i][a1] - contact[a1];
      std::uint32_t c = 0;
      if (rel[i][0] >= -ext[a0]) {
        if (rel[i][0] > ext[a0]) c = 2;
      } else {
        c = 1;
      }
      if (rel[i][1] >= -ext[a1]) {
        if (rel[i][1] > ext[a1]) c |= 8;
      } else {
        c |= 4;
      }
      if (c == 0) break;
      codes[i] = c;
      ++outside;
    }
    if (outside != 3) continue;
    if ((codes[0] & codes[1] & codes[2]) != 0) return 0;
    if (((codes[0] | codes[1] | codes[2]) & 3) == 0) {
      // No vertex is outside on a0 — test edges crossing the a1 faces.
      std::uint32_t mask = 3;
      for (int i = 0; i < 3 && mask != 0; ++i) {
        const int j = (i + 1) % 3;
        const std::uint32_t diff = codes[i] ^ codes[j];
        if (diff & 4) { // edge crosses a1 = -ext[a1]
          const float t =
              ((rel[j][0] - rel[i][0]) * (-ext[a1] - rel[i][1])) /
                  (rel[j][1] - rel[i][1]) +
              rel[i][0];
          if (-ext[a0] <= t && t <= ext[a0]) mask = 0;
        }
        if (diff & 8) { // edge crosses a1 = +ext[a1]
          const float t =
              ((rel[j][0] - rel[i][0]) * (ext[a1] - rel[i][1])) /
                  (rel[j][1] - rel[i][1]) +
              rel[i][0];
          if (-ext[a0] <= t && t <= ext[a0]) mask = 0;
        }
      }
      if (mask != 0) return 0;
    } else {
      // Some vertex is outside on a0 — test edges crossing the a0 faces.
      std::uint32_t mask = 0xc;
      for (int i = 0; i < 3 && mask != 0; ++i) {
        const int j = (i + 1) % 3;
        if ((codes[i] & 3) == 0) mask &= codes[i];
        const std::uint32_t diff = codes[i] ^ codes[j];
        if (diff & 1) { // edge crosses a0 = -ext[a0]
          const float t =
              ((rel[j][1] - rel[i][1]) * (-ext[a0] - rel[i][0])) /
                  (rel[j][0] - rel[i][0]) +
              rel[i][1];
          if (-ext[a1] <= t) {
            if (t <= ext[a1]) {
              mask = 0;
            } else {
              mask &= 8;
            }
          } else {
            mask &= 4;
          }
        }
        if (diff & 2) { // edge crosses a0 = +ext[a0]
          const float t =
              ((rel[j][1] - rel[i][1]) * (ext[a0] - rel[i][0])) /
                  (rel[j][0] - rel[i][0]) +
              rel[i][1];
          if (-ext[a1] <= t) {
            if (t <= ext[a1]) break; // inside face — projection overlaps
            mask &= 8;
          } else {
            mask &= 4;
          }
        }
      }
      if (mask != 0) return 0;
    }
  }
  return 1;
}

// ---------------------------------------------------------------------------
// FUN_004088cc / FUN_0040894c — plane pushout for the slide target
//   p: target point (in/out); n: node plane {nx,ny,nz,d}; m: margin
//   3D variant writes all axes; XY variant keeps p.z.
// ---------------------------------------------------------------------------

void pushout3D(float* p, const CollisionNode& n, float m) {
  float d = p[2] * n.nz + p[0] * n.nx + p[1] * n.ny + n.d;
  if (d <= m && -m <= d) {
    d = (d >= 0.0f) ? m - d : -d - m;
    p[0] = n.nx * d + p[0];
    p[1] = n.ny * d + p[1];
    p[2] = d * n.nz + p[2];
  }
}

void pushoutXY(float* p, const CollisionNode& n, float m) {
  float d = p[2] * n.nz + p[0] * n.nx + p[1] * n.ny + n.d;
  if (d <= m && -m <= d) {
    d = (d >= 0.0f) ? m - d : -d - m;
    p[0] = n.nx * d + p[0];
    p[1] = n.ny * d + p[1];
  }
}

// ---------------------------------------------------------------------------
// FUN_00425600 — point-in-triangle (dominant-axis projection + parity)
//   hit: the candidate point; v0/v1/v2: the triangle; n: its (signed) normal
// ---------------------------------------------------------------------------

int pointInTri(const float* hit, const float* v0, const float* v1,
               const float* v2, const float* n) {
  int a0, a1;
  const float ax = std::fabs(n[0]), ay = std::fabs(n[1]), az = std::fabs(n[2]);
  if (ax <= ay) {
    if (az < ay) {
      a0 = 0;
      a1 = 2;
    } else {
      a0 = 0;
      a1 = 1;
    }
  } else if (az < ax) {
    a0 = 1;
    a1 = 2;
  } else {
    a0 = 0;
    a1 = 1;
  }
  float rel[3][2];
  rel[0][0] = v0[a0] - hit[a0];
  rel[0][1] = v0[a1] - hit[a1];
  rel[1][0] = v1[a0] - hit[a0];
  rel[1][1] = v1[a1] - hit[a1];
  rel[2][0] = v2[a0] - hit[a0];
  rel[2][1] = v2[a1] - hit[a1];
  int sign = (rel[0][1] >= 0.0f) ? 1 : -1;
  int parity = 0;
  int to = 1;
  for (int from = 0;;) {
    if (to == 0) break;
    to = from + 1;
    if (to > 2) to = 0;
    const int vsign = (rel[to][1] < 0.0f) ? -1 : 1;
    if (vsign == sign) {
      ++from;
      continue;
    }
    sign = vsign;
    if (rel[from][0] < 0.0f || rel[to][0] < 0.0f) {
      if (!(rel[from][0] < 0.0f && rel[to][0] < 0.0f) &&
          ((rel[to][0] - rel[from][0]) * rel[from][1]) /
                  (rel[to][1] - rel[from][1]) <
              rel[from][0]) {
        parity ^= 1;
      }
    } else {
      ++parity;
    }
    ++from;
  }
  return parity & 1;
}

// ---------------------------------------------------------------------------
// FUN_00413730 — segment vs triangle (local space)
//   end receives the hit point (the segment shortens per hit)
// ---------------------------------------------------------------------------

int segTri(const float* start, float* end, const float* verts,
           const std::uint16_t* tri) {
  const float* v0 = verts + (std::uint32_t)tri[0] * 3;
  const float* v1 = verts + (std::uint32_t)tri[1] * 3;
  const float* v2 = verts + (std::uint32_t)tri[2] * 3;
  const float nx = (v1[2] - v0[2]) * (v2[1] - v0[1]) -
                   (v1[1] - v0[1]) * (v2[2] - v0[2]);
  const float ny = (v1[0] - v0[0]) * (v2[2] - v0[2]) -
                   (v2[0] - v0[0]) * (v1[2] - v0[2]);
  const float nz = (v2[0] - v0[0]) * (v1[1] - v0[1]) -
                   (v1[0] - v0[0]) * (v2[1] - v0[1]);
  const float pd = -(nz * v0[2] + nx * v0[0] + ny * v0[1]);
  const float d0 = nz * start[2] + nx * start[0] + ny * start[1] + pd;
  if ((nz * end[2] + nx * end[0] + ny * end[1] + pd) * d0 > 0.0f) return 0;
  const float ddn = (end[2] - start[2]) * nz + (end[0] - start[0]) * nx +
                    (end[1] - start[1]) * ny;
  const float t = (std::fabs(ddn) == 0.0f) ? 0.0f : -(d0 / ddn);
  const float hx = (end[0] - start[0]) * t + start[0];
  const float hy = (end[1] - start[1]) * t + start[1];
  const float hz = (end[2] - start[2]) * t + start[2];
  const float n[3] = {nx, ny, nz};
  const float hit[3] = {hx, hy, hz};
  if (!pointInTri(hit, v0, v1, v2, n)) return 0;
  end[0] = hx;
  end[1] = hy;
  end[2] = hz;
  return 1;
}

// ---------------------------------------------------------------------------
// FUN_004138d8 — object-local segment probe (floor-probe helper)
//   start/end: world-space segment; end receives the world hit point
//   outElem: hit element index (-1 none); outTri: hit tri index
// ---------------------------------------------------------------------------

void objectProbe(const CollisionObject* obj, const float* start, float* end,
                 int* outElem, int* outTri) {
  const float* m = obj->xform;
  const float invS2 = 1.0f / (obj->scale * obj->scale);
  float ls[3], le[3];
  for (int i = 0; i < 3; ++i) {
    const float ds0 = start[0] - obj->origin[0];
    const float ds1 = start[1] - obj->origin[1];
    const float ds2 = start[2] - obj->origin[2];
    const float de0 = end[0] - obj->origin[0];
    const float de1 = end[1] - obj->origin[1];
    const float de2 = end[2] - obj->origin[2];
    ls[i] = (ds2 * m[6 + i] + ds0 * m[i] + ds1 * m[3 + i]) * invS2;
    le[i] = (de2 * m[6 + i] + de0 * m[i] + de1 * m[3 + i]) * invS2;
  }
  *outElem = -1;
  *outTri = -1;
  const CollisionElementSet* set = obj->elements;
  for (int e = 0; e < set->count; ++e) {
    if ((1u << (e & 31)) & obj->elemMaskB) continue;
    const CollisionElement& el = set->elems[e];
    for (int t = 0; t < el.triCount; ++t) {
      const std::uint16_t* tri =
          reinterpret_cast<const std::uint16_t*>(el.tris + t * 0x24);
      if (segTri(ls, le, el.verts, tri)) {
        *outElem = e;
        *outTri = t;
      }
    }
  }
  if (*outElem != -1) {
    for (int i = 0; i < 3; ++i) {
      end[i] = le[2] * m[i * 3 + 2] + le[0] * m[i * 3] +
               le[1] * m[i * 3 + 1] + obj->origin[i];
    }
  }
}

// forward decls — defined below alongside the sweep internals.
int pointInTri(const float* p, const float* v0, const float* v1,
               const float* v2, const float* nrm);
int segAabbResolve(const float* start, const float* target,
                   const float* box, float* outClamp, float* outAlt);

// ---------------------------------------------------------------------------
// FUN_0045cd38 — segment-vs-inflated-AABB overlap prefilter
//   per axis: segMax >= boxMin - ext  &&  segMin <= boxMax + ext
//   (non-strict — touching counts as overlap)
// ---------------------------------------------------------------------------

int segAabbOverlap(const float* a, const float* b, const float* box,
                   const float* ext) {
  for (int i = 0; i < 3; ++i) {
    const float lo = (a[i] < b[i]) ? a[i] : b[i];
    const float hi = (a[i] < b[i]) ? b[i] : a[i];
    if (box[i] - ext[i] > hi) return 0;
    if (box[i + 3] + ext[i] < lo) return 0;
  }
  return 1;
}

// ---------------------------------------------------------------------------
// FUN_00418c60 stab internals — globals 0x54b6c8..0x54b6f8.
//   cand1/cand2: the probed segment; hitPt: the node-plane crossing
//   point (written unconditionally at each crossed node, consumed by
//   the poly scan); hitPoly/hitNode: the containing records.
//   The 0x54b6f8 mode byte is always 0 through FUN_00418c60 — modes
//   1/2 come from FUN_00418ce8 (a different caller set, not ported).
// ---------------------------------------------------------------------------

struct StabState {
  const float* verts;            // 0x54b6cc
  const CollisionPoly* polys;    // 0x54b6dc
  const CollisionNode* nodes;    // 0x54b6d8
  const float* cand1;            // 0x54b6f0 — segment start
  const float* cand2;            // 0x54b6ec — segment end
  float hitPt[3];                // 0x54b6e0 — crossing point
  const CollisionPoly* hitPoly;  // 0x54b6d4
  const CollisionNode* hitNode;  // 0x54b6f4
  int dbgVisits = 0;             // MDK_TRACE_STAB diagnostic only
};

// FUN_004189c8 — interpolate the cand1->cand2 segment to the node
// plane: t = -dStart / dot(cand2-cand1, n) (t=1 on a degenerate
// denominator); out = cand1 + t*(cand2-cand1). The dot accumulates
// in the original's order: (dy*ny + dx*nx) + dz*nz.
void stabPlanePoint(StabState& s, float dStart, const CollisionNode* n) {
  const float f = (s.cand2[1] - s.cand1[1]) * n->ny +
                  (s.cand2[0] - s.cand1[0]) * n->nx +
                  (s.cand2[2] - s.cand1[2]) * n->nz;
  const float t = (f == 0.0f) ? 1.0f : -dStart / f;
  for (int i = 0; i < 3; ++i)
    s.hitPt[i] = s.cand1[i] + t * (s.cand2[i] - s.cand1[i]);
}

// FUN_00418930 — scan a node's {lo16 count, hi16 firstIdx} poly set
// for containment of the crossing point (skips flags&0x20 polys).
int stabPolyScan(StabState& s, const CollisionNode* node,
                 std::uint32_t set) {
  const int count = static_cast<int>(set & 0xffff);
  const CollisionPoly* rec = s.polys + (set >> 16);
  const float n[3] = {node->nx, node->ny, node->nz};
  for (int i = 0; i < count; ++i, ++rec) {
    if (rec->flags & 0x20) continue;
    const float* v0 = s.verts + (std::uint32_t)rec->v[0] * 3;
    const float* v1 = s.verts + (std::uint32_t)rec->v[1] * 3;
    const float* v2 = s.verts + (std::uint32_t)rec->v[2] * 3;
    if (pointInTri(s.hitPt, v0, v1, v2, n)) {
      s.hitPoly = rec;
      s.hitNode = node;
      return 1;
    }
  }
  return 0;
}

// FUN_00418a50 mode-1 form (FUN_00418ce8's variant, OBSERVED
// 0x418b0d..0x418c35): crossings on planes with |nz| < 0.5
// (C(0x495288)) are skipped outright, and the containment scan runs
// on ONE set — polysPos when nz >= +0.5, polysNeg when nz <= -0.5 —
// instead of both. The only mode-1 caller family is the yaw-offset
// floor probe (FUN_0045d71c via object op 0xec).
const CollisionNode* stabWalkMode1(StabState& s,
                                   const CollisionNode* node) {
  while (node) {
    const float dStart = s.cand1[1] * node->ny + s.cand1[0] * node->nx +
                         s.cand1[2] * node->nz + node->d;
    const float dEnd = s.cand2[1] * node->ny + s.cand2[0] * node->nx +
                       s.cand2[2] * node->nz + node->d;
    const CollisionNode* r = nullptr;
    if (dStart < 0.0f) {
      if (node->childFar >= 0)
        r = stabWalkMode1(s, s.nodes + node->childFar);
    } else {
      if (node->childNear >= 0)
        r = stabWalkMode1(s, s.nodes + node->childNear);
    }
    if (r) return r;
    if (dStart * dEnd >= 0.0f) return nullptr;
    if (std::fabs(node->nz) >= 0.5f) {
      stabPlanePoint(s, dStart, node);
      if (stabPolyScan(s, node,
                       node->nz >= 0.0f ? node->polysPos
                                        : node->polysNeg)) {
        return node;
      }
    }
    node = (dStart >= 0.0f)
               ? (node->childFar >= 0 ? s.nodes + node->childFar : nullptr)
               : (node->childNear >= 0 ? s.nodes + node->childNear : nullptr);
  }
  return nullptr;
}

// FUN_00418a50 — recursive BSP stab traversal. Descends the side
// containing cand1 first; on a strict plane crossing (dStart*dEnd < 0)
// interpolates the crossing point and scans polysPos then polysNeg;
// then iterates the opposite side. Returns the containing node.
const CollisionNode* stabWalk(StabState& s, const CollisionNode* node) {
  static const bool traceStab = std::getenv("MDK_TRACE_STAB") != nullptr;
  while (node) {
    if (traceStab && ++s.dbgVisits > 40) {
      std::fprintf(stderr,
          "  [stab] visits=%d node=%ld far=%d near=%d "
          "seg=(%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f)\n",
          s.dbgVisits, (long)(node - s.nodes),
          (int)node->childFar, (int)node->childNear,
          s.cand1[0], s.cand1[1], s.cand1[2],
          s.cand2[0], s.cand2[1], s.cand2[2]);
      if (s.dbgVisits > 2000) return nullptr;
    }
    // Original accumulation order: (ny*p.y + nx*p.x) + nz*p.z + d.
    const float dStart = s.cand1[1] * node->ny + s.cand1[0] * node->nx +
                         s.cand1[2] * node->nz + node->d;
    const float dEnd = s.cand2[1] * node->ny + s.cand2[0] * node->nx +
                       s.cand2[2] * node->nz + node->d;
    const CollisionNode* r = nullptr;
    if (dStart < 0.0f) {
      if (node->childFar >= 0)
        r = stabWalk(s, s.nodes + node->childFar);
    } else {
      if (node->childNear >= 0)
        r = stabWalk(s, s.nodes + node->childNear);
    }
    if (r) return r;
    if (dStart * dEnd >= 0.0f) return nullptr;
    stabPlanePoint(s, dStart, node);
    if (stabPolyScan(s, node, node->polysPos) ||
        stabPolyScan(s, node, node->polysNeg)) {
      return node;
    }
    node = (dStart >= 0.0f)
               ? (node->childFar >= 0 ? s.nodes + node->childFar : nullptr)
               : (node->childNear >= 0 ? s.nodes + node->childNear : nullptr);
  }
  return nullptr;
}

} // namespace

// FUN_0045cd38 / FUN_0045c838 / FUN_00418c60 — exported wrappers over
// the file-local originals (the object pass + grounding probe of the
// Phase 5M camera obstruction call FUN_00430bf8).
int collisionSegAabbOverlap(const float* a, const float* b,
                            const float* box6, const float* ext) {
  return segAabbOverlap(a, b, box6, ext);
}

int collisionSegAabbResolve(const float* start, const float* target,
                            const float* box6, float* outClamp,
                            float* outAlt) {
  return segAabbResolve(start, target, box6, outClamp, outAlt);
}

const CollisionNode* collisionStab(const CollisionArena& arena,
                                   const float* from, const float* to,
                                   float* outPos) {
  return collisionStabFull(arena, from, to, outPos, nullptr);
}

const CollisionNode* collisionStabFull(const CollisionArena& arena,
                                       const float* from, const float* to,
                                       float* outPos,
                                       const CollisionPoly** outPoly) {
  if (outPoly) *outPoly = nullptr;
  if (!arena.verts || !arena.nodes) return nullptr;
  StabState s;
  s.verts = arena.verts;
  s.polys = arena.polys;
  s.nodes = arena.nodes;
  s.cand1 = from;
  s.cand2 = to;
  s.hitPt[0] = s.hitPt[1] = s.hitPt[2] = 0.0f;
  s.hitPoly = nullptr;
  s.hitNode = nullptr;
  const CollisionNode* r = stabWalk(s, arena.nodes);
  if (r) {
    std::memcpy(outPos, s.hitPt, 3 * sizeof(float));
    if (outPoly) *outPoly = s.hitPoly;
  }
  return r;
}

// FUN_00418ce8 export — the mode-1 stab (0x54b6f8=1): same walk as
// collisionStabFull but the nz>=0.5 floor gate and single-set scan
// of stabWalkMode1. Used by the script yaw-offset floor probe.
const CollisionNode* collisionStabMode1(const CollisionArena& arena,
                                        const float* from,
                                        const float* to) {
  if (!arena.verts || !arena.nodes) return nullptr;
  StabState s;
  s.verts = arena.verts;
  s.polys = arena.polys;
  s.nodes = arena.nodes;
  s.cand1 = from;
  s.cand2 = to;
  return stabWalkMode1(s, arena.nodes);
}

// FUN_004138d8 export — same body as the file-local floor-probe
// helper; kept under one implementation so both callers share the
// original's local-frame tri scan + end-point writeback.
void collisionObjectProbe(const CollisionObject* obj, const float* start,
                          float* end, int* outElem, int* outTri) {
  objectProbe(obj, start, end, outElem, outTri);
}

namespace {


// Sweep scratch — the original's globals block at 0x4a2098..0x4a2120.
struct SweepState {
  CollisionState* cs;
  const float* verts;          // a20f4
  const CollisionPoly* polys;  // a20ac
  const CollisionNode* nodes;  // a209c
  float pos[3];                // a20e8..f0
  float target[3];             // a20b0..b8
  float totalDelta[3];         // a20cc..d4
  float delta[3];              // a20dc..e4
  float ext[3];                // a2108..110
  float maxTravelSq;           // a20c0
  float fatMargin;             // a20a8
  float bestT;                 // a20a0
  int slideItersLeft;          // a20a4
  int slideProduced;           // a20c4
  const CollisionNode* hitNode;  // a20c8
  const CollisionPoly* hitPoly;  // a20d8
  float hitPt[3];              // a20f8..100
  float slideTarget[3];        // a2114..11c
};

int polyScan(SweepState& s, std::uint32_t set, const float* contact,
             const CollisionPoly** out) {
  const int count = (int)(set & 0xffff);
  const CollisionPoly* rec = s.polys + (set >> 16);
  for (int i = 0; i < count; ++i, ++rec) {
    if (rec->flags & 0x20) continue;
    const float* v0 = s.verts + (std::uint32_t)rec->v[0] * 3;
    const float* v1 = s.verts + (std::uint32_t)rec->v[1] * 3;
    const float* v2 = s.verts + (std::uint32_t)rec->v[2] * 3;
    if (boxTri(contact, s.ext, s.ext, v0, v1, v2)) {
      if (out) *out = rec;
      return 1;
    }
  }
  if (out) *out = nullptr;
  return 0;
}

// ---------------------------------------------------------------------------
// FUN_00408260 — recursive BSP sweep traversal
// ---------------------------------------------------------------------------

void bspSweep(SweepState& s, const CollisionNode* node) {
  for (;;) {
    const float margin = std::fabs(s.ext[2] * node->nz) +
                         std::fabs(s.ext[0] * node->nx) +
                         std::fabs(s.ext[1] * node->ny);
    const float dStart = s.pos[1] * node->ny + node->d +
                         s.pos[2] * node->nz + s.pos[0] * node->nx;
    std::uint8_t sideStart = 0;
    if (-s.fatMargin <= dStart) {
      sideStart = 1;
      if (node->childNear >= 0)
        bspSweep(s, s.nodes + node->childNear);
    }
    if (dStart <= s.fatMargin) {
      sideStart |= 2;
      if (node->childFar >= 0)
        bspSweep(s, s.nodes + node->childFar);
    }
    const float dTarget = s.target[1] * node->ny + node->d +
                          s.target[2] * node->nz + s.target[0] * node->nx;
    std::uint8_t sideTarget = 0;
    if (-s.fatMargin <= dTarget) sideTarget = 1;
    if (dTarget <= s.fatMargin) sideTarget |= 2;
    if ((sideStart | sideTarget) == 3 &&
        ((dStart < 0.0f && dStart <= dTarget) ||
         (0.0f <= dStart && dTarget <= dStart))) {
      float dFace;
      if (0.0f <= dStart) {
        dFace = margin;
        if (dStart < margin) dFace = dStart;
      } else {
        dFace = -margin;
        if (-margin < dStart) dFace = dStart;
      }
      const float dDelta = s.delta[2] * node->nz + s.delta[0] * node->nx +
                           s.delta[1] * node->ny;
      if (dDelta != 0.0f) {
        float t = -(dStart - dFace) / dDelta;
        if (t <= s.bestT && t <= 1.0f) {
          float contact[3] = {t * s.delta[0] + s.pos[0],
                              t * s.delta[1] + s.pos[1],
                              t * s.delta[2] + s.pos[2]};
          const std::uint32_t set =
              (dStart < 0.0f) ? node->polysNeg : node->polysPos;
          const CollisionPoly* tok = nullptr;
          if (polyScan(s, set, contact, &tok) != 1) {
            float t2 = -dStart / dDelta;
            if (t2 > 1.0f) t2 = 1.0f;
            contact[0] = t2 * s.delta[0] + s.pos[0];
            contact[1] = t2 * s.delta[1] + s.pos[1];
            contact[2] = t2 * s.delta[2] + s.pos[2];
            if (polyScan(s, set, contact, &tok) != 1) goto descend;
            t = t2;
          }
          s.hitPoly = tok;
          s.hitNode = node;
          if (s.slideItersLeft != 0) {
            const float dnTotal = s.totalDelta[2] * node->nz +
                                  s.totalDelta[0] * node->nx +
                                  s.totalDelta[1] * node->ny;
            if (dnTotal * dnTotal <= s.maxTravelSq) {
              const float rem = (1.0f - t) * dDelta;
              s.slideTarget[0] = s.target[0] - rem * node->nx;
              s.slideTarget[1] = s.target[1] - rem * node->ny;
              s.slideProduced = 1;
              if (((0.0f <= node->nz && node->nz < 0.75f && dDelta <= 0.0f) ||
                   (node->nz <= 0.0f && -0.75f < node->nz &&
                    0.0f <= dDelta))) {
                s.slideTarget[2] = s.target[2];
                pushoutXY(s.slideTarget, *node, margin + 0.01f);
              } else {
                s.slideTarget[2] = s.target[2] - rem * node->nz;
                pushout3D(s.slideTarget, *node, margin + 0.01f);
              }
            } else {
              s.slideProduced = 0;
              s.slideTarget[0] = contact[0];
              s.slideTarget[1] = contact[1];
              s.slideTarget[2] = contact[2];
            }
          }
          s.bestT = t;
          s.hitPt[0] = contact[0];
          s.hitPt[1] = contact[1];
          s.hitPt[2] = contact[2];
        }
      }
    }
  descend:
    sideStart = (std::uint8_t)(sideTarget & (sideStart ^ sideTarget));
    if (sideStart & 1) {
      if (node->childNear < 0) return;
      node = s.nodes + node->childNear;
    } else if (sideStart & 2) {
      if (node->childFar < 0) return;
      node = s.nodes + node->childFar;
    } else {
      return;
    }
  }
}

} // namespace

// ---------------------------------------------------------------------------
// FUN_00407fc0 — iterative sweep orchestrator
// ---------------------------------------------------------------------------

const CollisionPoly* collisionSweep(CollisionState& cs, const float* start,
                                    const float* target, int flag,
                                    const CollisionArena& arena,
                                    const float* ext, float scale,
                                    float* outPos,
                                    const CollisionNode** outNode) {
  if (outNode) *outNode = nullptr;
  if (!arena.verts || !arena.nodes) {
    std::memcpy(outPos, target, 3 * sizeof(float));
    return nullptr;
  }
  SweepState s;
  s.cs = &cs;
  s.verts = arena.verts;
  s.polys = arena.polys;
  s.nodes = arena.nodes;
  std::memcpy(s.pos, start, sizeof(s.pos));
  std::memcpy(s.target, target, sizeof(s.target));
  for (int i = 0; i < 3; ++i) s.totalDelta[i] = target[i] - start[i];
  std::memcpy(s.ext, ext, sizeof(s.ext));
  s.slideProduced = 1;
  s.hitNode = nullptr;
  s.hitPoly = nullptr;
  s.maxTravelSq =
      scale * (s.totalDelta[2] * s.totalDelta[2] +
               s.totalDelta[1] * s.totalDelta[1] +
               s.totalDelta[0] * s.totalDelta[0]);
  s.fatMargin = (std::fabs(s.ext[2]) + std::fabs(s.ext[0]) +
                 std::fabs(s.ext[1])) *
                2.0f;
  while (flag >= 0 && s.slideProduced) {
    s.hitPoly = nullptr;
    s.slideItersLeft = (flag != 0) ? 1 : 0;
    for (int i = 0; i < 3; ++i) s.delta[i] = s.target[i] - s.pos[i];
    s.slideProduced = 0;
    s.bestT = 5000.0f;
    bspSweep(s, s.nodes);
    if (s.slideProduced) {
      std::memcpy(s.target, s.slideTarget, sizeof(s.target));
      std::memcpy(s.pos, s.hitPt, sizeof(s.pos));
      if (cs.contactHook) {
        std::memcpy(cs.sweepContact, s.hitPt, sizeof(s.hitPt));
        cs.contactHook(cs, s.hitNode, s.hitPoly);
      }
    } else if (s.hitPoly && cs.contactHook) {
      std::memcpy(cs.sweepContact, s.hitPt, sizeof(s.hitPt));
      cs.contactHook(cs, s.hitNode, s.hitPoly);
    }
    --flag;
  }
  if (s.bestT >= 100.0f) {
    std::memcpy(outPos, s.target, sizeof(s.target));
    return nullptr;
  }
  std::memcpy(outPos, s.hitPt, sizeof(s.hitPt));
  if (outNode) *outNode = s.hitNode;
  return s.hitPoly;
}

// ---------------------------------------------------------------------------
// FUN_004630d4 — swept collision query + apply
// ---------------------------------------------------------------------------

const CollisionPoly* collisionApply(CollisionState& cs, float dx, float dy,
                                    float dz, float scale, const float* extVec,
                                    const CollisionNode** outNode) {
  static const float kExtHoriz[3] = {0.6f, 0.6f, 2.5f}; // DAT_0049bab4
  static const float kExtVert[3] = {0.4f, 0.4f, 2.5f};  // DAT_0049baa8
  const CollisionPoly* contact = nullptr;
  float margin = 0.01f;
  const float* ext = extVec;
  if (!ext) ext = (dz == 0.0f) ? kExtHoriz : kExtVert;
  const float oldX = cs.pos[0];
  const float oldY = cs.pos[1];
  if (dz == 0.0f) margin = 0.5f;
  const float oldZ = cs.pos[2] + ext[2] + margin;
  float appliedX = dx, appliedY = dy, appliedZ = dz;
  float resolved[3] = {oldX + dx, oldY + dy, oldZ + dz};
  if (!cs.queryEnabled || !cs.arena) {
    if (outNode) *outNode = nullptr;
  }
  if (!cs.queryEnabled) {
    // No static sweep — objects may still resolve below.
  } else if (cs.arena) {
    const float start[3] = {oldX, oldY, oldZ};
    float target[3] = {oldX + dx, oldY + dy, oldZ + dz};
    contact = collisionSweep(cs, start, target, 4, *cs.arena, ext, scale,
                             resolved, outNode);
    if (!contact && cs.carrier && !cs.carrierBusy && cs.carrierValid &&
        !cs.excludeObj) {
      float ctarget[3] = {resolved[0], resolved[1], resolved[2]};
      float cstart[3] = {oldX, oldY, oldZ};
      contact = collisionSweep(cs, cstart, ctarget, 0, *cs.carrier, ext, scale,
                               resolved, outNode);
    }
    appliedX = resolved[0] - oldX;
    appliedY = resolved[1] - oldY;
    appliedZ = resolved[2] - oldZ;
  }
  // Player AABB refresh around the pre-move position (0x540c30..c44).
  cs.playerBox[0] = oldX - ext[0];
  cs.playerBox[3] = ext[0] + oldX;
  cs.playerBox[1] = oldY - ext[1];
  cs.playerBox[4] = ext[1] + oldY;
  cs.playerBox[2] = oldZ - ext[2];
  cs.playerBox[5] = ext[2] + oldZ;
  if (cs.arenaValid && cs.objectDataLoaded && cs.arena) {
    // Swept query box = player box expanded by the applied delta.
    float qbox[6];
    std::memcpy(qbox, cs.playerBox, sizeof(qbox));
    if (appliedX < 0.0f) {
      qbox[0] += appliedX;
    } else {
      qbox[3] += appliedX;
    }
    if (appliedY >= 0.0f) {
      qbox[4] += appliedY;
    } else {
      qbox[1] += appliedY;
    }
    if (appliedZ >= 0.0f) {
      qbox[5] += appliedZ;
    } else {
      qbox[2] += appliedZ;
    }
    float objTarget[3] = {oldX + appliedX, oldY + appliedY, oldZ + appliedZ};
    const float segStart[3] = {oldX, oldY, oldZ};
    int hits = 0;
    for (const CollisionObject* obj = cs.arena->objects; obj;
         obj = obj->next) {
      if (!obj->named || !obj->model || (obj->flags148 & 0x810) != 0 ||
          obj == cs.excludeObj) {
        continue;
      }
      float obox[6];
      std::memcpy(obox, qbox, sizeof(obox));
      if (obj == cs.rideObj) {
        obox[2] = cs.pos[2] + 1.0f;
      } else {
        obox[2] = cs.playerBox[2];
        if (appliedZ < 0.0f) obox[2] += appliedZ;
      }
      if (!aabbOverlap(obox, obj->aabb)) continue;
      const CollisionElementSet* set = obj->elements;
      for (int e = 0; e < set->count; ++e) {
        if ((obj->flags14a & 0x10) != 0 &&
            ((1u << (e & 31)) & obj->elemMaskA) != 0) {
          continue;
        }
        if (((1u << (e & 31)) & obj->elemMaskB) != 0) continue;
        const float* ea = set->elems[e].aabb;
        const float ebox[6] = {ea[0] - ext[0], ea[1] - ext[1], ea[2] - ext[2],
                               ea[3] + ext[0], ea[4] + ext[1], ea[5] + ext[2]};
        if (!aabbOverlap(obox, ebox)) continue;
        cs.lastObjContact = obj;
        float clampPt[3], altPt[3];
        const int rc = segAabbResolve(segStart, objTarget, ebox, clampPt, altPt);
        if (rc != 0) {
          if (hits == 0 || rc == 2) {
            std::memcpy(objTarget, altPt, sizeof(objTarget));
          } else {
            std::memcpy(objTarget, clampPt, sizeof(objTarget));
          }
          ++hits;
        }
      }
    }
    if (!cs.queryEnabled || hits == 0) {
      appliedX = objTarget[0] - oldX;
      appliedY = objTarget[1] - oldY;
      appliedZ = objTarget[2] - oldZ;
    } else {
      collisionSweep(cs, segStart, objTarget, 0, *cs.arena, ext, scale,
                     resolved, nullptr);
      appliedX = resolved[0] - oldX;
      appliedY = resolved[1] - oldY;
      appliedZ = resolved[2] - oldZ;
    }
  }
  cs.pos[0] += appliedX;
  cs.pos[1] += appliedY;
  cs.pos[2] += appliedZ;
  return contact;
}

// ---------------------------------------------------------------------------
// FUN_00435eec — per-frame floor/contact probe
// ---------------------------------------------------------------------------

void collisionFloorProbe(CollisionState& cs) {
  int elemIdx = -1, triIdx = -1;
  float top[3] = {cs.pos[0], cs.pos[1],
                  (float)((double)cs.pos[2] + 3.0)};
  float bot[3] = {cs.pos[0], cs.pos[1],
                  (float)((double)cs.pos[2] - 3.0)};
  cs.contactFlags &= ~0x2;
  cs.floorObj = nullptr;
  if (cs.rideObj && !(cs.rideObj->flags14a & 0x80) && cs.rideActive) {
    if (cs.dismountHook) cs.dismountHook(cs);
    cs.rideActive = 0;
  }
  if (!cs.rideObj || !cs.rideActive) {
    const CollisionObject* hitObj = nullptr;
    std::uint32_t hitMask = 0;
    if (cs.arena) {
      for (const CollisionObject* obj = cs.arena->objects; obj;
           obj = obj->next) {
        if (!obj->named || !obj->model || (obj->flags148 & 0x10) != 0 ||
            !(obj->flags149 & 1)) {
          continue;
        }
        objectProbe(obj, top, bot, &elemIdx, &triIdx);
        if (elemIdx >= 0) {
          hitMask = 1u << (elemIdx & 31);
          hitObj = obj;
        }
      }
    }
    if (hitObj) {
      cs.floorObj = hitObj;
      cs.floorZ = bot[2];
      cs.floorOffset = bot[2] - hitObj->baseZ;
      cs.contactFlags |= 2;
      cs.floorElemMask = hitMask;
      return;
    }
    cs.rideObj = nullptr;
    if (cs.rideActive) {
      if (cs.dismountHook) cs.dismountHook(cs);
      cs.rideActive = 0;
    }
  } else {
    cs.floorElemMask = cs.rideElemMask;
    cs.floorZ = cs.rideObj->baseZ + cs.floorOffset;
    cs.contactFlags |= 2;
    cs.floorObj = cs.rideObj;
  }
}

// ---------------------------------------------------------------------------
// FUN_00419ee0 — level-stream collision blob parse
// ---------------------------------------------------------------------------

namespace {

std::uint32_t blobU32(const std::uint8_t* p) {
  std::uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

float blobF32(const std::uint8_t* p) {
  float v;
  std::memcpy(&v, p, 4);
  return v;
}

} // namespace

bool collisionBlobParse(const std::uint8_t* blob, std::size_t size,
                        CollisionArena* arena,
                        std::uint32_t outCounts[4]) {
  if (!blob || !arena || size < 8) return false;
  const std::uint8_t* const end = blob + size;

  const std::uint32_t cA = blobU32(blob);
  // A records are 10 bytes each (uVar1*10 BYTE arithmetic in the
  // original), then a 2-byte alignment pad iff countA is odd.
  std::size_t off = 4 + static_cast<std::size_t>(cA) * 10;
  if (cA & 1) off += 2;
  if (blob + off + 4 > end) return false;

  const std::uint32_t cB = blobU32(blob + off);
  const std::uint8_t* const nodeBase = blob + off + 4;
  off += 4 + static_cast<std::size_t>(cB) * 44;
  if (cB == 0 || blob + off + 4 > end) return false;

  const std::uint32_t cC = blobU32(blob + off);
  const std::uint8_t* const polyBase = blob + off + 4;
  off += 4 + static_cast<std::size_t>(cC) * 36;
  if (cC == 0 || blob + off + 4 > end) return false;

  const std::uint32_t cD = blobU32(blob + off);
  const std::uint8_t* const vertBase = blob + off + 4;
  off += 4 + static_cast<std::size_t>(cD) * 12 + 4;
  if (cD == 0 || blob + off > end) return false;

  // The tables are aliased as struct arrays — require the dword
  // alignment the original stream always has.
  const auto aligned = [](const void* p) {
    return (reinterpret_cast<std::uintptr_t>(p) & 3) == 0;
  };
  if (!aligned(nodeBase) || !aligned(polyBase) || !aligned(vertBase)) {
    return false;
  }

  // Full validation — this parser feeds real files, so every record
  // is checked against the proven runtime invariants, not just the
  // header chain:
  //   nodes: ~unit split plane; children in [-1, countB); each
  //     poly set {lo16 count, hi16 firstIdx} stays inside the poly
  //     table (count 0 = unused; firstIdx 0xffff is the filler).
  //   polys: every u16 vertex index < countD.
  //   verts: finite f32 triples.
  for (std::uint32_t i = 0; i < cB; ++i) {
    const std::uint8_t* nb = nodeBase + i * 44;
    const float nx = blobF32(nb), ny = blobF32(nb + 4),
                nz = blobF32(nb + 8);
    const float len2 = nx * nx + ny * ny + nz * nz;
    if (!(len2 > 0.81f && len2 < 1.21f)) return false;
    std::int16_t ch[2];
    std::memcpy(ch, nb + 16, 4);
    if (ch[0] < -1 || ch[1] < -1 || ch[0] >= (int)cB ||
        ch[1] >= (int)cB) {
      return false;
    }
    for (int s = 0; s < 2; ++s) {
      const std::uint32_t ps = blobU32(nb + 20 + s * 4);
      const std::uint32_t cnt = ps & 0xffff, fst = ps >> 16;
      if (cnt && fst + cnt > cC) return false;
    }
  }
  for (std::uint32_t i = 0; i < cC; ++i) {
    std::uint16_t v[3];
    std::memcpy(v, polyBase + i * 36, 6);
    if (v[0] >= cD || v[1] >= cD || v[2] >= cD) return false;
  }
  for (std::uint32_t i = 0; i < cD * 3; ++i) {
    if (!std::isfinite(blobF32(vertBase + i * 4))) return false;
  }

  arena->verts = reinterpret_cast<const float*>(vertBase);
  arena->polys = reinterpret_cast<const CollisionPoly*>(polyBase);
  arena->nodes = reinterpret_cast<const CollisionNode*>(nodeBase);
  // OBSERVED (FUN_004320d0, called at 0x4323dd and 0x42911c right
  // after the FUN_00419ee0 install): the record's +0x446..+0x45a
  // AABB is folded from the installed vertex array. +0x44e — the
  // unaligned minZ — is the abyss reference read by the object kill
  // plane (pos.z < +0x44e - 200 at 0x45bdd6), the death snap
  // (+0x44e - 150 at 0x4583ab/0x4583ce), 0x45fd55, and the player
  // failsafe (0x4673f3). The write goes through
  // `lea ecx,[eax+0x446]` + [ecx+8]/[ecx+0x14] stores. Arenas whose
  // +0x24 vert pointer is NULL keep the memset-zero reference.
  float minZ = blobF32(vertBase + 8);
  for (std::uint32_t i = 1; i < cD; ++i) {
    const float z = blobF32(vertBase + i * 12 + 8);
    if (z < minZ) minZ = z;
  }
  arena->deepFloorZ = minZ;
  if (outCounts) {
    outCounts[0] = cA;
    outCounts[1] = cB;
    outCounts[2] = cC;
    outCounts[3] = cD;
  }
  return true;
}

} // namespace mdk
