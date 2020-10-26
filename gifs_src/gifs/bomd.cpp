#include "properties.hpp"
#include "constants.hpp"
#include "bomd.hpp"
#include "qm_qchem.hpp"
//
#include <armadillo>
//
QMInterface*
select_interface(ConfigBlockReader& reader,
                 FileHandle& fh,
                 const arma::uvec& qmids, 
	             arma::mat& qm_crd, 
	             arma::mat& mm_crd, 
	             arma::vec& mm_chg)
{ 
    std::string qmcode{};
    int mult, chg;
    size_t excited_states, min_state;
    reader.get_data("qmcode", qmcode);
    reader.get_data("multiplicity", mult);
    reader.get_data("charge", chg);
    
    reader.get_data("excited_states", excited_states);
    reader.get_data("min_state", min_state);

    
    if (qmcode == "qchem") {
        return new QM_QChem(fh, qmids, qm_crd, mm_crd, mm_chg, chg, mult, excited_states, min_state);
    } else {
        throw "qm interface  not implemented!";
    }
}

void
BOMD::add_qm_keys(ConfigBlockReader& reader) 
{
    reader.add_entry("qmcode", "qchem");
    reader.add_entry("charge", 0);
    reader.add_entry("multiplicity", 1);
    reader.add_entry("excited_states", (size_t) 0);
    reader.add_entry("min_state", (size_t) 1); // for (A)FSSH only
};



BOMD::BOMD(arma::mat& qm_grd,
           arma::mat& mm_grd) :
    qm_grd{qm_grd}, mm_grd{mm_grd}, energy(1)
{};

void
BOMD::setup(FileHandle& fh,
            const arma::uvec& atomicnumbers,
            arma::mat& qm_crd, 
            arma::mat& mm_crd, 
            arma::vec& mm_chg
        ) {
  auto reader = setup_reader();
  add_qm_keys(reader);
  reader.parse(fh);
  get_reader_data(reader);
  qm = select_interface(reader, fh, atomicnumbers, qm_crd, mm_crd, mm_chg);
};


ConfigBlockReader
BOMD::setup_reader() {
    //using types = ConfigBlockReader::types;
    ConfigBlockReader reader{"bomd"};
    reader.add_entry("active_state", (size_t) 0);
    return reader;
};


void
BOMD::get_reader_data(ConfigBlockReader& reader) {
  reader.get_data("active_state", active_state);

  // consistency checks
  size_t excited_states = 0;
  reader.get_data("excited_states", excited_states);
  if (active_state > excited_states){
    std::cerr << "active_state = " << active_state << " < " << excited_states << " = excited_states!" << std::endl; 
    throw std::logic_error("The active state must be within the excited states!");
  }

  if (active_state > 1e3){
    std::cerr << "active_state=" << active_state << "! This is probably a mistake." << std::endl;
  }

  energy.set_size(excited_states + 1);
};


double 
BOMD::update_gradient()
{
    qm->update();
    // FIXME: wire-in support for active-state selection
    PropMap props{};
    props.emplace(QMProperty::qmgradient, &qm_grd);
    props.emplace(QMProperty::mmgradient, &mm_grd);
    props.emplace(QMProperty::energies, {active_state}, &energy);
    //
    qm->get_properties(props);
    //
    return energy[active_state];
};


bool BOMD::rescale_velocities(arma::mat &velocities, arma::vec &masses, arma::mat &total_gradient, double total_energy) {
  (void) total_gradient; (void) masses; (void) velocities;
  
  edrift = (total_energy - elast)/elast;
  elast = total_energy;
  std::cout << "Total Energy: " << elast << "; Fractional drift: " << edrift << std::endl;
  return false;
};
