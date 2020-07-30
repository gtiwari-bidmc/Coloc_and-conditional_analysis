#pragma once

#include <algorithm>
#include <complex>
#include <cstring>
#include <iostream>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <numeric>
#include <vector>
#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <sys/stat.h>
#include "dcdflib.h"

#define FLOATERR std::numeric_limits<double>::epsilon();

bool file_exists(const std::string &name);
void ShowWarning(const std::string msg, bool verbose);
void ShowError(const std::string msg);
void checkEntry(std::string txt, double *val);

bool isFloatEqual(double lhs, double rhs);
std::string string2upper(const std::string &str);
double pchisq(double x, double df);

double v_calc_median(const std::vector<double> &x);
std::vector<std::size_t> v_sort_indices(const std::vector<std::string> &v);
void eigenVector2Vector(Eigen::VectorXd &x, std::vector<double> &y);
double logsum(const std::vector<double> &x);
double logdiff(double x, double y);

std::vector<double> lm(const std::vector<double> &x, const std::vector<double> &y);
std::vector<double> lm_fixed(const std::vector<double> &x, const std::vector<double> &y);

//template<typename KeyType, typename LeftValue, typename RightValue>
//std::map<KeyType, std::pair<LeftValue, RightValue> > IntersectMaps(const std::map<KeyType, LeftValue> &left, const std::map<KeyType, RightValue> &right);

//std::map<std::string, int> vm_intersect(const std::map<std::string, int> &left, const std::vector<std::string> &right);
