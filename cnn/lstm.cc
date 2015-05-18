#include "cnn/lstm.h"

#include <string>
#include <cassert>
#include <vector>
#include <iostream>

#include "cnn/nodes.h"
#include "cnn/training.h"

using namespace std;

namespace cnn {

enum { X2I, H2I, C2I, BI, X2O, H2O, C2O, BO, X2C, H2C, BC };

LSTMBuilder::LSTMBuilder(unsigned layers,
                       unsigned input_dim,
                       unsigned hidden_dim,
                       Model* model) : hidden_dim(hidden_dim), layers(layers), zeros(hidden_dim, 0) {
  unsigned layer_input_dim = input_dim;
  for (unsigned i = 0; i < layers; ++i) {
    // i
    Parameters* p_x2i = model->add_parameters(Dim({hidden_dim, layer_input_dim}));
    Parameters* p_h2i = model->add_parameters(Dim({hidden_dim, hidden_dim}));
    Parameters* p_c2i = model->add_parameters(Dim({hidden_dim, hidden_dim}));
    Parameters* p_bi = model->add_parameters(Dim({hidden_dim}));
    
    // o
    Parameters* p_x2o = model->add_parameters(Dim({hidden_dim, layer_input_dim}));
    Parameters* p_h2o = model->add_parameters(Dim({hidden_dim, hidden_dim}));
    Parameters* p_c2o = model->add_parameters(Dim({hidden_dim, hidden_dim}));
    Parameters* p_bo = model->add_parameters(Dim({hidden_dim}));

    // c
    Parameters* p_x2c = model->add_parameters(Dim({hidden_dim, layer_input_dim}));
    Parameters* p_h2c = model->add_parameters(Dim({hidden_dim, hidden_dim}));
    Parameters* p_bc = model->add_parameters(Dim({hidden_dim}));
    layer_input_dim = hidden_dim;  // output (hidden) from 1st layer is input to next

    vector<Parameters*> ps = {p_x2i, p_h2i, p_c2i, p_bi, p_x2o, p_h2o, p_c2o, p_bo, p_x2c, p_h2c, p_bc};
    params.push_back(ps);
  }  // layers
}

void LSTMBuilder::new_graph(ComputationGraph* cg) {
  sm.transition(RNNOp::new_graph);
  param_vars.clear();

  for (unsigned i = 0; i < layers; ++i) {
    string layer = to_string(i);
    auto& p = params[i];

    // i
    VariableIndex i_x2i = cg->add_parameter(p[X2I]);
    VariableIndex i_h2i = cg->add_parameter(p[H2I]);
    VariableIndex i_c2i = cg->add_parameter(p[C2I]);
    VariableIndex i_bi = cg->add_parameter(p[BI]);

    // o
    VariableIndex i_x2o = cg->add_parameter(p[X2O]);
    VariableIndex i_h2o = cg->add_parameter(p[H2O]);
    VariableIndex i_c2o = cg->add_parameter(p[C2O]);
    VariableIndex i_bo = cg->add_parameter(p[BO]);

    // c
    VariableIndex i_x2c = cg->add_parameter(p[X2C]);
    VariableIndex i_h2c = cg->add_parameter(p[H2C]);
    VariableIndex i_bc = cg->add_parameter(p[BC]);

    vector<VariableIndex> vars = {i_x2i, i_h2i, i_c2i, i_bi, i_x2o, i_h2o, i_c2o, i_bo, i_x2c, i_h2c, i_bc};
    param_vars.push_back(vars);
  }
}

void LSTMBuilder::start_new_sequence(ComputationGraph* cg,
                                     vector<VariableIndex> c_0,
                                     vector<VariableIndex> h_0) {
  sm.transition(RNNOp::start_new_sequence);
  h.clear();
  c.clear();
  h0 = h_0;
  c0 = c_0;
  if (h0.empty() || c0.empty()) {
    VariableIndex zero_input = cg->add_input(Dim({hidden_dim}), &zeros);
    if (c0.empty()) { c0 = vector<VariableIndex>(layers, zero_input); }
    if (h0.empty()) { h0 = vector<VariableIndex>(layers, zero_input); }
  }
  assert (h0.size() == layers);
  assert (c0.size() == layers);
}

VariableIndex LSTMBuilder::add_input(VariableIndex x, ComputationGraph* cg) {
  sm.transition(RNNOp::add_input);
  const unsigned t = h.size();
  h.push_back(vector<VariableIndex>(layers));
  c.push_back(vector<VariableIndex>(layers));
  vector<VariableIndex>& ht = h.back();
  vector<VariableIndex>& ct = c.back();
  VariableIndex in = x;
  for (unsigned i = 0; i < layers; ++i) {
    const vector<VariableIndex>& vars = param_vars[i];
    VariableIndex i_h_tm1;
    VariableIndex i_c_tm1;
    if (t == 0) {
      // intial value for h and c at timestep 0 in layer i
      // defaults to zero matrix input if not set in add_parameter_edges
      i_h_tm1 = h0[i];
      i_c_tm1 = c0[i];
    } else {  // t > 0
      i_h_tm1 = h[t-1][i];
      i_c_tm1 = c[t-1][i];
    }
    // input
    VariableIndex i_ait = cg->add_function<AffineTransform>({vars[BI], vars[X2I], in, vars[H2I], i_h_tm1, vars[C2I], i_c_tm1});
    VariableIndex i_it = cg->add_function<LogisticSigmoid>({i_ait});
    // forget
    VariableIndex i_ft = cg->add_function<ConstantMinusX>({i_it}, 1.f);
    // write memory cell
    VariableIndex i_awt = cg->add_function<AffineTransform>({vars[BC], vars[X2C], in, vars[H2C], i_h_tm1});
    VariableIndex i_wt = cg->add_function<Tanh>({i_awt});
    // output
    VariableIndex i_nwt = cg->add_function<CwiseMultiply>({i_it, i_wt});
    VariableIndex i_crt = cg->add_function<CwiseMultiply>({i_ft, i_c_tm1});
    ct[i] = cg->add_function<Sum>({i_crt, i_nwt}); // new memory cell at time t
 
    VariableIndex i_aot = cg->add_function<AffineTransform>({vars[BO], vars[X2O], in, vars[H2O], i_h_tm1, vars[C2O], ct[i]});
    VariableIndex i_ot = cg->add_function<LogisticSigmoid>({i_aot});
    VariableIndex ph_t = cg->add_function<Tanh>({ct[i]});
    in = ht[i] = cg->add_function<CwiseMultiply>({i_ot, ph_t});
  }
  return ht.back();
}

} // namespace cnn