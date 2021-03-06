#include <ctime>
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <algorithm>
#include "qm_qchem.hpp"
#include "properties.hpp"
#include <armadillo>
#include <unordered_map>

ConfigBlockReader
QM_QChem::qchem_reader() {
  using types = ConfigBlockReader::types;
  ConfigBlockReader reader{"qchem"};
  reader.add_entry("qc_scratch", "DEFAULT");
  reader.add_entry("basis", types::STRING);
  reader.add_entry("exchange", types::STRING);
  reader.add_entry("scf_algorithm", "DIIS");
  // FIXME: add scf_guess and default to read
  reader.add_entry("nthreads", 1);
  // FIXME: ConfigReader should support bool
  reader.add_entry("singlets", 1);
  reader.add_entry("triplets", 0);
  reader.add_entry("state_analysis", 0);
  reader.add_entry("spin_flip", 0);
  reader.add_entry("save_nacvector", 0);
  reader.add_entry("record_spectrum", 0);
  return reader;
}


/*
  Recall that Q-Chem can decide to change the number of excited states
  it computes! Usually this will be to a larger number of states. --Y.S.

  Happens in setman; to see where, search for "NRoots was altered as:"

  On subsequent invocation (as during AIMD), setman_init.F will reset
  the number requested to the original number.

  FIXME: we need to fix the above for our AFSSH routines.
*/
QM_QChem::QM_QChem(FileHandle& fh, 
           const arma::uvec& in_qmids, 
		   arma::mat& in_qm_crd, 
		   arma::mat& in_mm_crd, 
		   arma::vec& in_mm_chg, 
		   const int charge, 
		   const int mult,
		   const int excited_states,
                   const int min_state):
  QMInterface(in_qmids, in_qm_crd, in_mm_crd, in_mm_chg, charge, mult, excited_states, min_state)
{
  /*
    The environmental variables $QCSCRATCH, $QCTHREADS shall take
    precedence over anything set in the config.
  */
  
  ConfigBlockReader reader = qchem_reader();
  reader.parse(fh);

  reader.get_data("basis", basis_set);
  reader.get_data("exchange", exchange_method);
  reader.get_data("scf_algorithm", scf_algorithm);

  {
    int in;
    reader.get_data("singlets", in);
    singlets = in;
    reader.get_data("triplets", in);
    triplets = in;
    reader.get_data("state_analysis", in);
    state_analysis = in;
    reader.get_data("spin_flip", in);
    spin_flip = in;
    reader.get_data("save_nacvector", in);
    save_nacvector = in;
    reader.get_data("record_spectrum", in);
    record_spectrum = in;
  }

  if (singlets && triplets){
    std::cerr << "WARNING: selecting singlets and triplets together not supported!" << std::endl;
  }
  
  std::string conf_scratch;
  reader.get_data("qc_scratch", conf_scratch);
  qc_scratch_directory = get_qcscratch(conf_scratch);
  
  /*
    Setting $QCTHREADS seems sufficient on the Subotnik cluster, but
    perhaps this is unique to our setup. Need to check with Evgeny to
    be sure. Changes to $OMP_NUM_THREADS seem to have no effect.
  */
  int conf_threads;
  reader.get_data("nthreads", conf_threads);
  if (conf_threads > 1){
    setenv("QCTHREADS", std::to_string(conf_threads).c_str(), 0);
  }

  qc_executable = get_qcprog();
}


void QM_QChem::get_properties(PropMap &props){
  for (QMProperty p: props.keys()){
    switch(p){
      
    case QMProperty::wfoverlap:
      get_wf_overlap(props.get(QMProperty::wfoverlap));
      break;

    case QMProperty::diabatic_rot_mat:
      get_diabatic_rot_mat(props.get(QMProperty::diabatic_rot_mat));
      break;

    case QMProperty::nacvector_imag:
      throw std::invalid_argument("Imaginary NAC not implemented!");
      break;

    case QMProperty::nacvector:{
      const arma::uvec &idx = *props.get_idx(QMProperty::nacvector);
      size_t A = idx[0]; size_t B = idx[1];
      get_nac_vector(props.get(QMProperty::nacvector), A, B);
      called(S::energy); called(S::ex_energy);
      break;
    }

    case QMProperty::mmgradient:
      break; // if we need mm, then we will do qm, which catches both
    case QMProperty::qmgradient:{
      arma::mat *g_qm = props.get(QMProperty::qmgradient);
      arma::uword surface = 0; //assume ground state
      called(S::energy);
      if (props.has_idx(QMProperty::qmgradient)){
	surface = (*props.get_idx(QMProperty::qmgradient))[0];
	if (surface){called(S::ex_energy);}
      }
      
      if (props.has(QMProperty::mmgradient)){
	arma::mat *g_mm = props.get(QMProperty::mmgradient);
	get_gradient(*g_qm, *g_mm, surface);
      }
      else{
	get_gradient(*g_qm, surface);
      }
      break;
    }

    case QMProperty::mmgradient_multi:
      break; // if we need mm, then we will do qm, which catches both
    case QMProperty::qmgradient_multi:{
      arma::cube *g_qm = props.get(QMProperty::qmgradient_multi);
      arma::uvec surfaces = arma::regspace<arma::uvec>(0,excited_states); //assume all
      if (props.has_idx(QMProperty::qmgradient_multi)){ // specific states
	surfaces = *props.get_idx(QMProperty::qmgradient_multi);
      }

      if (g_qm->n_slices != surfaces.n_elem){
	throw std::range_error("Insufficient space for requested gradients!");
      }
      
      arma::uword i=0;
      for (arma::uword surface: surfaces){
	if (props.has(QMProperty::mmgradient_multi)){
	  arma::cube *g_mm = props.get(QMProperty::mmgradient_multi);
	  get_gradient(g_qm->slice(i), g_mm->slice(i), surface);
	}
	else{
	  get_gradient(g_qm->slice(i), surface);
	}

	i++;
      }
      called(S::energy); called(S::ex_energy);
      break;
    }
      
    case QMProperty::energies:{
      arma::vec * energies = props.get(QMProperty::energies); 
      if (props.has_idx(QMProperty::energies)){
	if (energies->n_elem != excited_states + 1){
	  throw std::range_error("Insufficient space for all energies!");
	}
      	get_all_energies(energies);
      }
      else{
      	get_ground_energy(energies); // FIXME: combine ground/excited calls
      }
      break;
    }

    default: // Compile with the default case commented out and the compiler will detect neglected properties
      throw std::invalid_argument("Unknown QMProperty!");
      break;
    }
  }

  if (state_analysis){
    do_state_analysis();
  }

  if (record_spectrum){
    do_record_spectrum();
  }
}


void QM_QChem::do_record_spectrum(void){
  arma::vec e(1);
  if (!called(S::ex_energy)){
    // dummy call to make sure the transition dipoles and energies are written
    get_all_energies(&e); 
  }

  e.set_size(excited_states);
  arma::mat mu(4, excited_states);
  // for mu, the first row is the oscillator strength
  
  if (excited_states != readQFMan(FILE_SET_ENERGY, e.memptr(), excited_states, FILE_POS_BEGIN)){
    throw std::runtime_error("Unable to parse excited energies");
  }

  if( excited_states * 4 != readQFMan(FILE_TRANS_DIP_MOM, mu.memptr(), excited_states * 4, FILE_POS_BEGIN)){
    throw std::runtime_error("Unable to parse transition dipole moments");
  }
  
  //write spectrum to file as [excitation energy] [strength]
  {

    // if (spin_flip && !triplets){
    //   e = e - e[0]; // the true ground state is the first excited state
    // }

    if (e[0] < 0){ // the true ground state is the first excited state
      e = e - e[0];
    }
    
    arma::mat spec(excited_states,2);
    spec.col(0) = e;
    spec.col(1) = mu.row(0).t();
    std::string specf = get_qcwdir() + "/" + "spectrum.dat";

    std::ofstream stream;
    stream.open(specf, std::ios::out | std::ios::app | std::ios::binary);
    if (!stream){
      throw std::runtime_error("Cannot open spectrum file, " + specf);
    }
    else{
      spec.save(stream, arma::raw_ascii);
    }
  }

}


void QM_QChem::do_state_analysis(void){
  REMKeys keys = {{"jobtype","sp"},
                  {"make_cube_files", "false"},
                  {"gui", "2"},
                  {"state_analysis", "true"},
                  {"molden_format", "true"}};

  REMKeys ex = excited_rem();
  keys.insert(ex.begin(), ex.end());
  if (called(S::ex_energy)){
    keys.insert({{"skip_setman", "1"},
                 {"skip_scfman", "1"}});
  }
  else{
    keys.insert({{"cis_rlx_dns", "1"}});
  }

  std::ofstream input = get_input_handle();
  write_rem_section(input, keys);  
  write_molecule_section(input);
  input.close();

  {
    std::string guifile = get_qcwdir() + "/" +
      std::to_string(call_idx()) + ".fchk";
    setenv("GUIFILE", guifile.c_str(), 1);
  }
  
  exec_qchem();
}


/*
  FIXME: Use another REM variable to save PREV_GEO
  FIXME: Make sure this works with spin_flip methods
*/
void QM_QChem::get_wf_overlap(arma::mat *U){  
  if (! called(S::wfoverlap)){
    REMKeys k = excited_rem();
    k.insert({{"jobtype","sp"},
    	      {"namd_lowestsurface",std::to_string(min_state)},
    	      {"dump_wf_overlap", "1"}});

    std::ofstream input = get_input_handle();
    write_rem_section(input, k);
    write_molecule_section(input);
    input.close();
    exec_qchem();
  }
  else{
    //don't recompute 
  }

  if (call_idx() < 2){ // if this is the first call, don't actually read from $QC; there will be no overlap to read
    U->eye();
  }
  else{
    size_t count = readQFMan(FILE_WF_OVERLAP, U->memptr(), excited_states*excited_states, FILE_POS_BEGIN);
    if (count != excited_states * excited_states){
      throw std::runtime_error("Unable to parse wavefunction overlap");
    }
  }
}


/*
  FIXME: Need to recitfy min_state or choose which states to mix
  FIXME: Want to choose between ER and Boys and ...
  FIXME: Want to pick separate spin states (rem_boys_cis_spin_separate)
  FIXME: May want to increase REM_CIS_CONVERGENCE to 7 (from default 6)
  FIXME: Do we need to include $localised_diabatization section?
*/
void QM_QChem::get_diabatic_rot_mat(arma::mat *U){
  REMKeys k = excited_rem();
  k.insert({{"jobtype","sp"},
	    {"boys_cis_numstate", std::to_string(excited_states)}
	    });

  std::ofstream input = get_input_handle();
  write_rem_section(input, k);
  write_molecule_section(input);
  input.close();
  exec_qchem();
  
  size_t count = readQFMan(FILE_DIAB_ROT_MAT, U->memptr(), excited_states*excited_states, FILE_POS_BEGIN);
  if (count != excited_states * excited_states){
    throw std::runtime_error("Unable to parse diabatic rotation matrix");
  }
}


void QM_QChem::get_ground_energy(arma::vec *e){
  // Build job if we need to
  if (! called(S::energy)){
    std::ofstream input = get_input_handle();
    write_rem_section(input, {{"jobtype","sp"}});
    write_molecule_section(input);
    input.close();
    exec_qchem();
  }

  parse_energies(*e);
}

// Gets all energies: ground + excited states
void QM_QChem::get_all_energies(arma::vec *e){
  
  if (! called(S::ex_energy)){
    REMKeys k = excited_rem();
    k.insert({{"jobtype","sp"}});
    
    std::ofstream input = get_input_handle();
    write_rem_section(input, k);
    write_molecule_section(input);
    input.close();
    exec_qchem();
  }

  parse_energies(*e);
}


void QM_QChem::get_gradient(arma::mat &g_qm, arma::mat &g_mm, arma::uword surface){
  get_gradient(g_qm, surface);
  if (NMM > 0){
    parse_mm_gradient(g_mm);
  }
}


void QM_QChem::get_gradient(arma::mat &g_qm, arma::uword surface){
  if (surface > excited_states){
    throw std::invalid_argument("Requested surface not computed!");
  }

  std::ofstream input = get_input_handle();
  REMKeys keys = {{"jobtype","force"}};

  if (surface != 0){
    REMKeys ex = excited_rem();
    keys.insert(ex.begin(), ex.end());
    keys.insert({{"cis_state_deriv", std::to_string(surface)}});
    if (called(S::ex_grad)){
      keys.insert({{"skip_setman", "1"},
	           {"skip_scfman", "1"}});
    }
    else{
      keys.insert({{"cis_rlx_dns", "1"}});
    }
  }
  
  write_rem_section(input, keys);  
  write_molecule_section(input);
  input.close();

  exec_qchem();

  parse_qm_gradient(g_qm);
}


// Should we split mm/qm nac?
void QM_QChem::get_nac_vector(arma::mat *nac, size_t A, size_t B){
  REMKeys k = excited_rem();
  k.insert({{"jobtype","sp"},
	    {"calc_nac", "true"},
	    {"cis_der_numstate", "2"}});//Always, in our case, between 2 states

  if (NMM > 0){
    k.insert({{"nac_pointcharge", "1"},
              {"qm_mm", "true"}});
  }
  
  std::ofstream input = get_input_handle();
  write_rem_section(input, k);

  /*
    Section format:
    $derivative_coupling
      comment line
      A B ...
    $end
    where A, B, ... are the states between which to compute the
    derivative coupling. The $rem section must include
    cis_der_numstates equal to the total number of states
    specified. The ground state is 0.

  */
  input << "$derivative_coupling" << std::endl;
  input << "comment" << std::endl;
  input << A << " " << B << std::endl;
  input << "$end" << std::endl;
  
  write_molecule_section(input);
  input.close();

  exec_qchem();

  size_t count = readQFMan(FILE_DERCOUP, nac->memptr(), 3 * (NMM + NQM), FILE_POS_BEGIN);
  if (count != 3 * (NMM + NQM)){
    throw std::runtime_error("Unable to parse NAC vector!");
  }

  //
  if (save_nacvector){
    std::string nacf = get_qcwdir() + "/" +
      "nacvector." + std::to_string(call_idx());
    nac->save(nacf);
  }
}



void QM_QChem::parse_energies(arma::vec &e){  
  readQFMan(FILE_ENERGY, e.memptr(), 1, FILE_POS_CRNT_TOTAL_ENERGY);

  if (excited_states > 0 && e.n_elem > 1){
    double e_ground = e[0];

    if (!(e.n_elem >= excited_states + 1)){
      throw std::range_error("Insufficient space for all excited states in vec e!");
    }
    
    size_t count = readQFMan(FILE_SET_ENERGY, e.memptr() + 1, excited_states, FILE_POS_BEGIN);
    if (count != excited_states){
      throw std::runtime_error("Unable to parse energies!");
    }
        
    // Energies for higher states given in terms of exitations so we
    // must add in the ground.
    e += e_ground;
    e[0] = e_ground;
  }
}


void QM_QChem::parse_qm_gradient(arma::mat &g_qm){
  size_t count = readQFMan(FILE_NUCLEAR_GRADIENT, g_qm.memptr(), 3*NQM, FILE_POS_BEGIN);
  if (count != 3 * NQM){
    throw std::runtime_error("Unable to parse QM gradient!");
  }
}


void QM_QChem::parse_mm_gradient(arma::mat &g_mm){
  /* Set a ceiling on the number of doubles we read-in because the
     number of MM atoms can fluctuate during a simulation. */
  size_t count = readQFMan(FILE_EFIELD, g_mm.memptr(), 3*NMM, FILE_POS_BEGIN);

  if (count != 3 * NMM){
    throw std::runtime_error("Unable to parse MM gradient!");
  }

  for (size_t i = 0; i < NMM; i++){
    const double q = chg_mm[i];
    // FILE_EFIELD really contains the *field* so we need a negative to get the gradient.
    g_mm.col(i) *= -1.0 * q;
  }
  /*
    FIXME: prevent efield.dat from being written with REM_QMMM_EXT_GIFS set

    This method replaced parsing efield.dat, which has the following
    format:

    Ex(mm1) Ey(mm1) Ez(mm1)
    ...
    Ex(mmN) Ey(mmN) Ez(mmN)
    Ex(qm1) Ey(qm1) Ez(qm1)
    ...
    Ex(qmN) Ey(qmN) Ez(qmN)

    where Ea(u) is the component of the electric field in the a
    direction at u and mmi and qmi are the coordinates of the ith mm
    and qm atoms respectively. N.N. The product of the field and the
    charge is a force rather than the gradient.
  */
}


const std::string QM_QChem::get_qcprog(void){
  char * qc_str = std::getenv("QC");
  std::string default_path = "qcprog.exe";
  if (nullptr == qc_str){
    std::cerr << "Warning, $QC not set; using " << default_path << std::endl;
    return default_path;
  }
  else{
    return std::string(qc_str) + "/exe/qcprog.exe";
  }
}


const std::string QM_QChem::get_qcwdir(void){
  char * pwd = std::getenv("PWD");
  std::string qcwdir = std::string(pwd) + "/GQSH";

  static bool create = true;

  if (create){
    struct stat st = {};
    if (-1 == stat(qcwdir.c_str(), &st)){
      mkdir(qcwdir.c_str(), 0700);
    }
    create = false;
  }

  return qcwdir;
}

/*
  In order of preference:
  1)               If $QCSCRATCH is set and an absolute path use it
  2) Failing that, if conf_dir   is set and an absolute path use it
  3) Failing that, use our default path

  The above are set in reverse.
*/

const std::string QM_QChem::get_qcscratch(std::string conf_dir){
  char * pwd = std::getenv("PWD");
  std::string scratch_path = std::string(pwd) + "/GQSH.sav";

  // if the config directory is valid, take it as the default
  if (conf_dir[0] == '/'){
    scratch_path = conf_dir;
  }
  
  char * qc_str = std::getenv("QCSCRATCH");
  if (nullptr == qc_str){
    std::cerr << "Warning, $QCSCRATCH not set; using " << scratch_path << std::endl;
  }
  else if (qc_str[0] != '/'){
    std::cerr << "Warning, $QCSCRATCH is not an absolute path; instead using " << scratch_path << std::endl;
  }
  else{
    scratch_path = std::string(qc_str);
    // If using a global scratch path, make it unique
    // FIXME: could still collide if QCSCRATCH is on a network device
    std::srand(getpid());
    scratch_path += "/GIFS_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(rand());
  }

  /* Make sure the directory exists; create otherwise */
  struct stat st = {};
  if (-1 == stat(scratch_path.c_str(), &st)){
    mkdir(scratch_path.c_str(), 0700);
  }

  /*
    Set $QCSCRATCH to whatever we're using. Incidentally, this fixes a
     corner case where Q-Chem will crash if QCSCRATCH=""; set, but to
     the empty string.
  */
  setenv("QCSCRATCH", scratch_path.c_str(), true);
  std::cout << "taking scratch path to be: " << scratch_path << std::endl;

  
  return scratch_path;
}


// FIXME: redirect stderr too? 2>&1 >  
void QM_QChem::exec_qchem(void){
  std::string cmd = "cd " + get_qcwdir() + "; " + // change to  target WD
    qc_executable + " " +
    qc_scratch_directory + "/" + qc_input_file + " " +
    qc_scratch_directory + " >" + qc_log_file + " 2>&1";
  int status = std::system(cmd.c_str());
  if (status){
    throw std::runtime_error("Q-Chem could not be called or exited abnormally; see " + get_qcwdir() + "/" + qc_log_file);
  }
  first_call = false;
}


/* Consistent formatting and file name */
std::ofstream QM_QChem::get_input_handle(){
  std::ofstream os(qc_scratch_directory + "/" + qc_input_file);
  os.setf(std::ios_base::fixed, std::ios_base::floatfield);
  os.precision(std::numeric_limits<double>::digits10);

  return os;
}

REMKeys QM_QChem::excited_rem(void){
  // if (! (excited_states > 0)){
  //   throw std::logic_error("Cannot request multi-state property without excited states.");
  // }

  REMKeys excited
    {
      {"cis_n_roots", std::to_string(excited_states)},
      {"set_roots_orig", std::to_string(excited_states)}, // make sure we always use the same number of states
      {"max_cis_cycles", "500"}, // same as set_iter
      {"cis_singlets", std::to_string(singlets)},  
      {"cis_triplets", std::to_string(triplets)} 
    };

  return excited;
}

void QM_QChem::write_rem_section(std::ostream &os, const REMKeys &options){
  // Default options 
  REMKeys rem_keys
    {
     {"method",         exchange_method},
     {"basis",          basis_set},
     {"scf_algorithm",  scf_algorithm},
     {"sym_ignore",     "true"},
     {"qm_mm",          "true"},
     {"max_scf_cycles", "500"},
     {"skip_charge_self_interact", "1"},
     {"input_bohr",     "true"} // .../libgen/PointCharges.C works for MM charges
    };

  if (spin_flip){
    rem_keys.emplace("spin_flip", "1");
    // FIXME: perhaps only for record_spectrum?
    rem_keys.emplace("sts_mom", "1");
  }
  if (first_call){
    rem_keys.emplace("qmmm_ext_gifs", "1");
  }
  else{
    rem_keys.emplace("qmmm_ext_gifs", "2");
    rem_keys.emplace("scf_guess",     "read");
  }  

  /*
    Make sure passed options take precedence?  Could do this by
    inserting the defaults into the passed options rather than the
    other way around.
  */
  
  // merge in options; usually will include jobtype 
  rem_keys.insert(options.begin(), options.end());
  
  os << "$rem" << std::endl;
  for (const auto& e: rem_keys){
    os << e.first << " " << e.second << std::endl;
  }
  os << "$end" << std::endl;  
}


void QM_QChem::write_molecule_section(std::ostream &os){
  /*
    format of $molecule & $externall_charges sections:
    $molecule
    [charge] [multiplicity]
    [atomic-number] [x-coord] [y-coord] [x-coord]
    ...
    $end

    $external_charges
    [x-coord] [y-coord] [x-coord] [charge]
    ...
    $end
  */  
  os << "$molecule" << std::endl;
  os << qm_charge << " " << qm_multiplicity << std::endl;

  for (size_t i = 0; i < NQM; i++){
    os << atomids[i]      << " ";        // id
    os << crd_qm[i*3 + 0] << " ";        // x
    os << crd_qm[i*3 + 1] << " ";        // y
    os << crd_qm[i*3 + 2] << std::endl;  // x
  }
  os <<  "$end" << std::endl;

  if (NMM > 0){
    os << std::endl << "$external_charges" << std::endl;    
    for (size_t i = 0; i < NMM; i++){
      os << crd_mm[i*3 + 0] << " ";        // x
      os << crd_mm[i*3 + 1] << " ";        // y
      os << crd_mm[i*3 + 2] << " ";        // z
      os << chg_mm[i]       << std::endl;  // charge
    }
    os << "$end" << std::endl;
  }
}


/*
  Sentinel system to track whether the requested property has been
  computed since the last call to update(). See/update the protected
  nested enum class S (as in sentinel) for the list of properties

  FIXME: think about how to use QMProperty instead; might require a
  different keyword for energy.
*/
bool QM_QChem::called(S s){
  static std::unordered_map<S, int> calls;
  if(call_idx() == calls[s]){
    return true;
  }
  else{
    calls[s] = call_idx();
    return false;
  }
}

/*
  Given a q-qchem file number (see qm_qchem.hpp for examples), read N
  elements from the file (in the scratch directory) starting from the
  offset into memptr, which must point to a block of memory with space
  for at least N doubles.
*/
size_t QM_QChem::readQFMan(int filenum, double * memptr, size_t N, size_t offset){  
  std::string path = qc_scratch_directory + "/" + std::to_string(filenum) + ".0";
  std::ifstream ifile;
  ifile.open(path, std::ios::in | std::ios::binary);

  if (!ifile.is_open()){
    throw std::ios_base::failure("Error: cannot read from " + path);
  }

  char buffer[sizeof(double)];
  size_t i = 0;
  size_t count = 0;
  while(ifile.read(buffer, sizeof(double))){
    if ((i >= offset) && (i < offset + N)){
      double * d = (double *) buffer;
      *(memptr + count++) = *d;
    }
    i++;
  }
  ifile.close();

  return count;
}
