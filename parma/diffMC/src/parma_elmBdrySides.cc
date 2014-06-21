#include "parma_base.h"
#include "parma_sides.h"

namespace parma {  
  class ElmBdrySides : public Sides {
    public:
      ElmBdrySides(apf::Mesh* m) : Sides(m), totalSides(0) {
        init(m);
      }
      int total() {
        return totalSides;
      }
    private:
      int totalSides;
      void init(apf::Mesh* m) {
        apf::MeshEntity* s;
        apf::MeshIterator* it = m->begin(m->getDimension()-1);
        totalSides = 0;
        while ((s = m->iterate(it)))
          if (m->countUpward(s)==1 && m->isShared(s)) {
            const int peerId = apf::getOtherCopy(m,s).first;
            set(peerId, get(peerId)+1);
            ++totalSides;
          }
        m->end(it);
      }
  };

  Sides* makeElmBdrySides(apf::Mesh* m) {
    return new ElmBdrySides(m);
  }
}


