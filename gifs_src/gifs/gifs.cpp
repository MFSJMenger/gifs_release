#include <vector>
#include "gifs.hpp"
#include "gifs_implementation.hpp"
#include "properties.hpp"

//Don't inline these; they need to be exported and available to Gromacs etc...

void create_qm_interface(size_t nqm, const int* qm_atomids)
{
  Gifs gifs_handle(nqm, qm_atomids);
}


float gifs_get_forces(const float* qm_crd, size_t nmm, const float* mm_crd, const float* mm_chg, float* f, float* fshift)
{
  Gifs gifs_handle;
  /*
    FIXME: MFSJM: I don't think the corresponding call exists in GIFS
    yet---seems like it needs to be plummed through
    Conversion/LinkAtoms.
  */ 
  return gifs_handle.get_gradient(qm_crd, nmm, mm_crd, mm_chg, f, fshift);
};
