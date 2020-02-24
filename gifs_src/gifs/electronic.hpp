#ifndef __GIFS_ELECTRONIC_HPP
#define __GIFS_ELECTRONIC_HPP

#include <armadillo>

class Electronic{
public:
  Electronic(const arma::cx_mat &cs) : amplitudes (cs) {reserve();};
  Electronic(const arma::cx_vec &c) : amplitudes (c) {reserve();};
  Electronic(arma::uword NStates): Electronic(NStates, 1) {reserve();};
  Electronic(arma::uword NStates, arma::uword NSets) {amplitudes.set_size(NStates, NSets); reserve();};

  void set(const arma::cx_mat &cs) {amplitudes = cs;};
  void set(const arma::cx_vec &c) {amplitudes.col(0) = c;};

  arma::cx_vec get(void) const;
  
  void advance(const arma::cx_mat &H, double dt);
  
private:
  void reserve(void);

  arma::cx_mat amplitudes;
  arma::cx_mat k1, k2, k3, k4;  
};

#endif
