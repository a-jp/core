
/****************************************************************************** 

  Copyright 2013 Scientific Computation Research Center, 
      Rensselaer Polytechnic Institute. All rights reserved.
  
  The LICENSE file included with this distribution describes the terms
  of the SCOREC Non-Commercial License this program is distributed under.
 
*******************************************************************************/
#include <PCU.h>
#include "maShape.h"
#include "maSize.h"
#include "maAdapt.h"
#include "maSnap.h"
#include "maOperator.h"
#include "maEdgeSwap.h"
#include "maDoubleSplitCollapse.h"
#include "maFaceSplitCollapse.h"
#include "maShortEdgeRemover.h"
#include "maShapeHandler.h"
#include "maBalance.h"
#include "maDBG.h"
#include <pcu_util.h>

namespace ma {

/* projects vertex 3 onto the plane
   of the bottom triangle and returns
   the zone in which it lands as a bit code.
   Each bit indicates whether the area coordinate
   of that vertex is positive.
*/

int getSliverCode(
    Adapt* a,
    Entity* tet)
{
  SizeField* sf = a->sizeField;
  Mesh* m = a->mesh;
  Matrix J,Q;
  apf::MeshElement* me = apf::createMeshElement(m,tet);
  Vector center(.25,.25,.25);
  apf::getJacobian(me,center,J);
  sf->getTransform(me,center,Q);
  J = J*Q; //Jacobian in metric space
  apf::destroyMeshElement(me);
  Vector v03 = J[2];
  J[2] = apf::cross(J[0],J[1]); //face normal towards v[3]
  Vector projected = v03 - apf::project(v03,J[2]); //v[3] projected to face
  Matrix inverseMap = apf::invert(apf::transpose(J));
  Vector basisPoint = inverseMap * projected;
  Vector areaPoint(1-basisPoint[0]-basisPoint[1],
                   basisPoint[0],
                   basisPoint[1]);
  int code = 0;
  for (int i=0; i < 3; ++i)
    if (areaPoint[i] > 0)
      code |= (1<<i);
  PCU_ALWAYS_ASSERT(code);
  return code;
}

CodeMatch matchSliver(
    Adapt* a,
    Entity* tet)
{
  /* this table was auto-generated by the sliverCodeMatch program */
  CodeMatch const table[8] =
  {{-1,-1}
  ,{4,1}
  ,{1,1}
  ,{2,0}
  ,{2,1}
  ,{0,0}
  ,{1,0}
  ,{0,1}
  };
  return table[getSliverCode(a,tet)];
}

struct IsBadQuality : public Predicate
{
  IsBadQuality(Adapt* a_):a(a_) {}
  bool operator()(Entity* e)
  {
    return a->shape->getQuality(e) < a->input->goodQuality;
  }
  Adapt* a;
};

int markBadQuality(Adapt* a)
{
  IsBadQuality p(a);
  return markEntities(a, a->mesh->getDimension(), p, BAD_QUALITY, OK_QUALITY);
}

void unMarkBadQuality(Adapt* a)
{
  Mesh* m = a->mesh;
  Iterator* it;
  Entity* e;
  int count = 0;
  it = m->begin(m->getDimension());
  while ((e = m->iterate(it))) {
    if (getFlag(a, e, ma::BAD_QUALITY))
      clearFlag(a, e, ma::BAD_QUALITY);
    count++;
  }
  m->end(it);
}

double getMinQuality(Adapt* a)
{
  PCU_ALWAYS_ASSERT(a);
  Mesh* m;
  m = a->mesh;
  PCU_ALWAYS_ASSERT(m);
  Iterator* it = m->begin(m->getDimension());
  Entity* e;
  double minqual = 1;
  while ((e = m->iterate(it))) {
    if (!apf::isSimplex(m->getType(e)))
      continue;
    double qual = a->shape->getQuality(e);
    if (qual < minqual)
      minqual = qual;
  }
  m->end(it);
  return PCU_Min_Double(minqual);
}

class ShortEdgeFixer : public Operator
{
  public:
    ShortEdgeFixer(Adapt* a):
      remover(a)
    {
      adapter = a;
      mesh = a->mesh;
      sizeField = a->sizeField;
      shortEdgeRatio = a->input->maximumEdgeRatio;
      nr = nf = 0;
      element = 0;
    }
    virtual ~ShortEdgeFixer()
    {
    }
    virtual int getTargetDimension() {return mesh->getDimension();}
    virtual bool shouldApply(Entity* e)
    {
      if ( ! getFlag(adapter,e,BAD_QUALITY))
        return false;
      element = e;
      Downward edges;
      int n = mesh->getDownward(element,1,edges);
      double l[6] = {};
      for (int i=0; i < n; ++i)
        l[i] = sizeField->measure(edges[i]);
      double maxLength;
      double minLength;
      Entity* shortEdge;
      maxLength = minLength = l[0];
      shortEdge = edges[0];
      for (int i=1; i < n; ++i)
      {
        if (l[i] > maxLength) maxLength = l[i];
        if (l[i] < minLength)
        {
          minLength = l[i];
          shortEdge = edges[i];
        }
      }
      if ((maxLength/minLength) < shortEdgeRatio)
      {
        clearFlag(adapter,element,BAD_QUALITY);
        return false;
      }
      remover.setEdge(shortEdge);
      return true;
    }
    virtual bool requestLocality(apf::CavityOp* o)
    {
      return remover.requestLocality(o);
    }
    virtual void apply()
    {
      if (remover.run())
        ++nr;
      else
      {
        ++nf;
        clearFlag(adapter,element,BAD_QUALITY);
      }
    }
  private:
    Adapt* adapter;
    Mesh* mesh;
    Entity* element;
    SizeField* sizeField;
    ShortEdgeRemover remover;
    double shortEdgeRatio;
  public:
    int nr;
    int nf;
};

class TetFixerBase
{
  public:
    virtual void setTet(Entity** v) = 0;
    virtual bool requestLocality(apf::CavityOp* o) = 0;
    virtual bool run() = 0;
};

class FixBySwap : public TetFixerBase
{
  public:
    FixBySwap(Adapt* a):
      adapter(a)
    {
      mesh = a->mesh;
      edgeSwap = makeEdgeSwap(a);
      nes = nf = numToTry = 0;
      edges[0] = 0;
      edges[1] = 0;
      edges[2] = 0;
    }
    ~FixBySwap()
    {
      delete edgeSwap;
    }
    virtual void setTet(Entity** v)
    {
      Entity* tet = apf::findElement(mesh, apf::Mesh::TET, v);
      PCU_ALWAYS_ASSERT(tet);
      match = matchSliver(adapter, tet);
      Entity* dv[4];
      mesh->getDownward(tet, 0, dv);
      Entity* rv[4];
      rotateTet(dv,match.rotation,rv);

      enum { EDGE_EDGE, FACE_VERT };
      if (match.code_index==EDGE_EDGE) {
	Entity* ev[2];
	ev[0] = rv[0]; ev[1] = rv[2];
	edges[0] = findUpward(mesh, apf::Mesh::EDGE, ev);
	ev[0] = rv[1]; ev[1] = rv[3];
	edges[1] = findUpward(mesh, apf::Mesh::EDGE, ev);
	numToTry = 2;
      }
      else
      {
      	PCU_ALWAYS_ASSERT(match.code_index==FACE_VERT);
	apf::findTriDown(mesh,rv,edges);
	numToTry = 3;
      }
    }
    virtual bool requestLocality(apf::CavityOp* o)
    {
      return o->requestLocality(edges, numToTry);
    }
    virtual bool run()
    {
      for (int i=0; i < numToTry; ++i)
        if (edgeSwap->run(edges[i]))
        {
          ++nes;
          return true;
        }
      ++nf;
      return false;
    }
  private:
    Adapt* adapter;
    Mesh* mesh;
    Entity* edges[3];
    EdgeSwap* edgeSwap;
    CodeMatch match;
    int numToTry;
    int nes;
    int nf;
};

class FaceVertFixer : public TetFixerBase
{
  public:
    FaceVertFixer(Adapt* a):
      faceSplitCollapse(a)
    {
      mesh = a->mesh;
      edgeSwap = makeEdgeSwap(a);
      nes = nf = nfsc = 0;
      edges[0] = 0;
      edges[1] = 0;
      edges[2] = 0;
    }
    ~FaceVertFixer()
    {
      delete edgeSwap;
    }
    virtual void setTet(Entity** v)
    {
/* in this template, the bottom face and v[3]
   are too close, the key edges are those that bound
   face v(0,1,2) */
      apf::findTriDown(mesh,v,edges);
      face = apf::findUpward(mesh, apf::Mesh::TRIANGLE, edges);
      tet = apf::findElement(mesh, apf::Mesh::TET, v);
    }
    virtual bool requestLocality(apf::CavityOp* o)
    {
      return o->requestLocality(edges,3);
    }
    virtual bool run()
    {
      for (int i=0; i < 3; ++i)
        if (edgeSwap->run(edges[i]))
        {
          ++nes;
          return true;
        }
      if (faceSplitCollapse.run(face, tet))
      {
        ++nfsc;
        return true;
      }
      ++nf;
      return false;
    }
  private:
    Mesh* mesh;
    Entity* edges[3];
    Entity* face;
    Entity* tet;
    FaceSplitCollapse faceSplitCollapse;
    EdgeSwap* edgeSwap;
  public:
    int nes; /* number of edge swaps done */
    int nfsc; /* number of FSCs done */
    int nf; /* number of failures */
};

class EdgeEdgeFixer : public TetFixerBase
{
  public:
    EdgeEdgeFixer(Adapt* a):
      doubleSplitCollapse(a)
    {
      mesh = a->mesh;
      edgeSwap = makeEdgeSwap(a);
      nes = ndsc = nf = 0;
      sf = a->sizeField;
      edges[0] = 0;
      edges[1] = 0;
    }
    ~EdgeEdgeFixer()
    {
      delete edgeSwap;
    }
    virtual void setTet(Entity** v)
    {
/* in this template, the v[0]-v[2] amd v[1]-v[3]
   edges are too close. */
      Entity* ev[2];
      ev[0] = v[0]; ev[1] = v[2];
      edges[0] = findUpward(mesh, apf::Mesh::EDGE, ev);
      ev[0] = v[1]; ev[1] = v[3];
      edges[1] = findUpward(mesh, apf::Mesh::EDGE, ev);
    }
    virtual bool requestLocality(apf::CavityOp* o)
    {
      return o->requestLocality(edges,2);
    }
    virtual bool run()
    {
      for (int i=0; i < 2; ++i)
	if (edgeSwap->run(edges[i]))
        {
          ++nes;
          return true;
        }
      if (doubleSplitCollapse.run(edges))
      {
        ++ndsc;
        return true;
      }
      ++nf;
      return false;
    }
  private:
    Mesh* mesh;
    Entity* edges[2];
    EdgeSwap* edgeSwap;
    DoubleSplitCollapse doubleSplitCollapse;
    SizeField* sf;
  public:
    int nes;
    int ndsc;
    int nf;
};

class LargeAngleTetFixer : public Operator
{
  public:
    LargeAngleTetFixer(Adapt* a):
      edgeEdgeFixer(a),
      faceVertFixer(a)
    {
      adapter = a;
      mesh = a->mesh;
      tet = 0;
      fixer = 0;
    }
    virtual ~LargeAngleTetFixer()
    {
    }
    virtual int getTargetDimension() {return 3;}
    enum { EDGE_EDGE, FACE_VERT };
    virtual bool shouldApply(Entity* e)
    {
      if ( ! getFlag(adapter,e,BAD_QUALITY))
        return false;
      tet = e;
      CodeMatch match = matchSliver(adapter,e);
      if (match.code_index==EDGE_EDGE)
        fixer = &edgeEdgeFixer;
      else
      { PCU_ALWAYS_ASSERT(match.code_index==FACE_VERT);
        fixer = &faceVertFixer;
      }
      Entity* v[4];
      mesh->getDownward(e,0,v);
      Entity* rv[4];
      rotateTet(v,match.rotation,rv);
      fixer->setTet(rv);
      return true;
    }
    virtual bool requestLocality(apf::CavityOp* o)
    {
      return fixer->requestLocality(o);
    }
    virtual void apply()
    {
      if ( ! fixer->run())
        clearFlag(adapter,tet,BAD_QUALITY);
    }
  private:
    Adapt* adapter;
    Mesh* mesh;
    Entity* tet;
    TetFixerBase* fixer;
  public:
    EdgeEdgeFixer edgeEdgeFixer;
    FaceVertFixer faceVertFixer;
};

class LargeAngleTetAligner : public Operator
{
  public:
    LargeAngleTetAligner(Adapt* a):
      fixer(a)
    {
      adapter = a;
      mesh = a->mesh;
      tet = 0;
    }
    virtual ~LargeAngleTetAligner()
    {
    }
    virtual int getTargetDimension() {return 3;}
    virtual bool shouldApply(Entity* e)
    {
      if ( ! getFlag(adapter,e,BAD_QUALITY))
        return false;
      tet = e;
      /* PCU_ALWAYS_ASSERT(mesh->getType(e) == apf::Mesh::TET); */
      enum { EDGE_EDGE, FACE_VERT };
      CodeMatch match = matchSliver(adapter,e);
      if (match.code_index==EDGE_EDGE) {
        clearFlag(adapter,tet,BAD_QUALITY);
      	return false;
      }
      /* else */
      /* { PCU_ALWAYS_ASSERT(match.code_index==FACE_VERT); */
      /*   fixer = &faceVertFixer; */
      /* } */
      Entity* v[4];
      mesh->getDownward(e,0,v);
      fixer.setTet(v);
      return true;
    }
    virtual bool requestLocality(apf::CavityOp* o)
    {
      return fixer.requestLocality(o);
    }
    virtual void apply()
    {
      if ( ! fixer.run())
        clearFlag(adapter,tet,BAD_QUALITY);
    }
  private:
    Adapt* adapter;
    Mesh* mesh;
    Entity* tet;
    FixBySwap fixer;
};

class LargeAngleTriFixer : public Operator
{
  public:
    LargeAngleTriFixer(Adapt* a)
    {
      adapter = a;
      mesh = a->mesh;
      edgeSwap = makeEdgeSwap(a);
      ns = nf = 0;
      tri = 0;
      edge = 0;
    }
    virtual ~LargeAngleTriFixer()
    {
      delete edgeSwap;
    }
    virtual int getTargetDimension() {return 2;}
    virtual bool shouldApply(Entity* e)
    {
      if ( ! getFlag(adapter,e,BAD_QUALITY))
        return false;
      tri = e;
      // get the metric Q for angle computations
      SizeField* sf = adapter->sizeField;
      Matrix Q;
      apf::MeshElement* me = apf::createMeshElement(mesh, tri);
      Vector center(1./3.,1./3.,1./3.);
      sf->getTransform(me,center,Q);

      // pick the edge opposite to the largest angle (in metric) for swap
      Entity* edges[3];
      mesh->getDownward(e,1,edges);
      double minCos = 1.0;
      for (int i = 0; i < 3; i++) {
        Entity* current = edges[i%3];
        Entity*    next = edges[(i+1)%3];
        double cosAngle = apf::computeCosAngle(mesh, tri, current, next, Q);
        if (cosAngle < minCos) {
          minCos = cosAngle;
          edge = edges[(i+2)%3];
	}
      }
      return true;
    }
    virtual bool requestLocality(apf::CavityOp* o)
    {
      return o->requestLocality(&edge,1);
    }
    virtual void apply()
    {
        if (edgeSwap->run(edge))
        {
          ++ns;
          return;
        }
      ++nf;
      clearFlag(adapter,tri,BAD_QUALITY);
    }
  private:
    Adapt* adapter;
    Mesh* mesh;
    Entity* tri;
    Entity* edge;
    EdgeSwap* edgeSwap;
    int ns;
    int nf;
};

static double fixShortEdgeElements(Adapt* a)
{
  double t0 = PCU_Time();
  ShortEdgeFixer fixer(a);
  applyOperator(a,&fixer);
  double t1 = PCU_Time();
  return t1 - t0;
}

static void fixLargeAngleTets(Adapt* a)
{
  LargeAngleTetFixer fixer(a);
  applyOperator(a,&fixer);
  PCU_Debug_Open();
  PCU_Debug_Print("--fixLargeAngles: %d edge-edge successes by swap",fixer.edgeEdgeFixer.nes);
  PCU_Debug_Print("--fixLargeAngles: %d edge-edge successes by double-split-collapse",fixer.edgeEdgeFixer.ndsc);
  PCU_Debug_Print("--fixLargeAngles: %d edge-edge failures",fixer.edgeEdgeFixer.nf);
  PCU_Debug_Print("--fixLargeAngles: %d face-vert successes by swap",fixer.faceVertFixer.nes);
  PCU_Debug_Print("--fixLargeAngles: %d face-vert successes by face-split-collapse",fixer.faceVertFixer.nfsc);
  PCU_Debug_Print("--fixLargeAngles: %d face-vert failures",fixer.faceVertFixer.nf);
}

static void fixLargeAngleTris(Adapt* a)
{
  LargeAngleTriFixer fixer(a);
  applyOperator(a,&fixer);
}

static void alignLargeAngleTets(Adapt* a)
{
  LargeAngleTetAligner aligner(a);
  applyOperator(a,&aligner);
}

static void alignLargeAngleTris(Adapt* a)
{
  LargeAngleTriFixer aligner(a);
  applyOperator(a,&aligner);
}

static double fixLargeAngles(Adapt* a)
{
  double t0 = PCU_Time();
  if (a->mesh->getDimension()==3)
    fixLargeAngleTets(a);
  else
    fixLargeAngleTris(a);
  double t1 = PCU_Time();
  return t1 - t0;
}

static void alignLargeAngles(Adapt* a)
{
  if (a->mesh->getDimension()==3)
    alignLargeAngleTets(a);
  else
    alignLargeAngleTris(a);
}

void fixElementShapes(Adapt* a)
{
  if ( ! a->input->shouldFixShape)
    return;
  double t0 = PCU_Time();
  int count = markBadQuality(a);
  int originalCount = count;
  int prev_count;
  double time;
  int iter = 0;
  do {
    if ( ! count)
      break;
    prev_count = count;
    print("--iter %d of shape correction loop: #bad elements %d", iter, count);
    time = fixLargeAngles(a);
    /* We need to snap the new verts as soon as they are
     * created (to avoid future problems). At the moment
     * new verts are created only during 3D mesh adapt, so
     * we only run a bulk snap operation if the mesh is 3D.
     */
    if (a->mesh->getDimension() == 3)
      snap(a);
    count = markBadQuality(a);
    print("--fixLargeAngles       in %f seconds: #bad elements %d",time,count);
    time = fixShortEdgeElements(a);
    count = markBadQuality(a);
    print("--fixShortEdgeElements in %f seconds: #bad elements %d",time,count);
    if (count >= prev_count)
      unMarkBadQuality(a); // to make sure markEntities does not complain!
    // balance the mesh to avoid empty parts
    midBalance(a);
    print("--percent change in number of bad elements %f",
	((double) prev_count - (double) count) / (double) prev_count);
    iter++;
  } while(count < prev_count);
  double t1 = PCU_Time();
  print("bad shapes down from %d to %d in %f seconds",
        originalCount,count,t1-t0);
}

void alignElements(Adapt* a)
{
  int max_iter = 5;
  if ( ! a->input->shouldFixShape)
    return;
  double t0 = PCU_Time();
  int count = markBadQuality(a);
  int originalCount = count;
  int prev_count;
  int i = 0;
  do {
    if ( ! count)
      break;
    prev_count = count;
    alignLargeAngles(a);
    count = markBadQuality(a);
    ++i;
    if (count >= prev_count || i >= max_iter)
      unMarkBadQuality(a);
  } while(count < prev_count && i < max_iter);

  double t1 = PCU_Time();
  print("non-aligned elements down from %d to %d in %f seconds",
        originalCount,count,t1-t0);
}

void printQuality(Adapt* a)
{
  if ( ! a->input->shouldPrintQuality)
    return;
  double minqual = getMinQuality(a);
  print("worst element quality is %e", minqual);
}

}
