/* Copyright 2020 The TensorFlow Quantum Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow_quantum/core/src/circuit_parser_qsim.h"

#include <string>
#include <vector>

#include "../qsim/lib/circuit.h"
#include "../qsim/lib/fuser.h"
#include "../qsim/lib/fuser_basic.h"
#include "../qsim/lib/gates_cirq.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/numbers.h"
#include "cirq/google/api/v2/program.pb.h"
#include "tensorflow/core/lib/core/status.h"

namespace tfq {

using ::cirq::google::api::v2::Moment;
using ::cirq::google::api::v2::Operation;
using ::cirq::google::api::v2::Program;
using ::tensorflow::Status;

namespace {

typedef absl::flat_hash_map<std::string, std::pair<int, float>> SymbolMap;
typedef qsim::Cirq::GateCirq<float> QsimGate;
typedef qsim::Circuit<QsimGate> QsimCircuit;

inline Status ParseProtoArg(const Operation& op, const std::string& arg_name,
                            const SymbolMap& param_map, float* result) {
  // find arg_name in proto.
  // iterator<Map<str, Arg>>
  const auto arg_v = op.args().find(arg_name);
  if (arg_v == op.args().end()) {
    return Status(tensorflow::error::INVALID_ARGUMENT,
                  "Could not find arg: " + arg_name + " in op.");
  }
  // find proto arg field.
  // ::cirq::google::api::v2::Arg
  const auto proto_arg = arg_v->second;
  *result = proto_arg.arg_value().float_value();
  if (!proto_arg.symbol().empty()) {
    // find symbol value in param_map.
    const auto iter = param_map.find(proto_arg.symbol());
    if (iter == param_map.end()) {
      return Status(
          tensorflow::error::INVALID_ARGUMENT,
          "Could not find symbol in parameter map: " + proto_arg.symbol());
    }
    *result = iter->second.second;
  }
  return Status::OK();
}

// series of fixed signature gate builders.
// there is no need to error check for unparseable symbols
// or proto args not being present. Those errors are caught
// upstream.

// single qubit gate Create(time, q0)
inline Status SingleConstantGate(
    const Operation& op, const SymbolMap& param_map,
    const std::function<QsimGate(unsigned int, unsigned int)>& create_f,
    const unsigned time, QsimCircuit* circuit) {
  unsigned int q0;
  bool unused = absl::SimpleAtoi(op.qubits(0).id(), &q0);
  circuit->gates.push_back(create_f(time, q0));
  return Status::OK();
}

// two qubit gate Create(time, q0, q1)
inline Status TwoConstantGate(
    const Operation& op, const SymbolMap& param_map,
    const std::function<QsimGate(unsigned int, unsigned int, unsigned int)>&
        create_f,
    const unsigned time, QsimCircuit* circuit) {
  unsigned int q0, q1;
  bool unused = absl::SimpleAtoi(op.qubits(0).id(), &q0);
  unused = absl::SimpleAtoi(op.qubits(1).id(), &q1);
  circuit->gates.push_back(create_f(time, q0, q1));
  return Status::OK();
}

// single qubit eigen -> Create(time, q0, exponent, global_shift)
inline Status SingleEigenGate(
    const Operation& op, const SymbolMap& param_map,
    const std::function<QsimGate(unsigned int, unsigned int, float, float)>&
        create_f,
    const unsigned time, QsimCircuit* circuit) {
  unsigned int q0;
  bool unused;
  float exp, exp_s, gs;
  Status u;
  unused = absl::SimpleAtoi(op.qubits(0).id(), &q0);
  u = ParseProtoArg(op, "exponent", param_map, &exp);
  if (!u.ok()) {
    return u;
  }
  u = ParseProtoArg(op, "exponent_scalar", param_map, &exp_s);
  if (!u.ok()) {
    return u;
  }
  u = ParseProtoArg(op, "global_shift", param_map, &gs);
  if (!u.ok()) {
    return u;
  }

  circuit->gates.push_back(create_f(time, q0, exp * exp_s, gs));
  return Status::OK();
}

// two qubit eigen -> Create(time, q0, q1, exp, gs)
inline Status TwoEigenGate(
    const Operation& op, const SymbolMap& param_map,
    const std::function<QsimGate(unsigned int, unsigned int, unsigned int,
                                 float, float)>& create_f,
    const unsigned time, QsimCircuit* circuit) {
  unsigned int q0, q1;
  float exp, exp_s, gs;
  bool unused;
  Status u;
  unused = absl::SimpleAtoi(op.qubits(0).id(), &q0);
  unused = absl::SimpleAtoi(op.qubits(1).id(), &q1);

  u = ParseProtoArg(op, "exponent", param_map, &exp);
  if (!u.ok()) {
    return u;
  }
  u = ParseProtoArg(op, "exponent_scalar", param_map, &exp_s);
  if (!u.ok()) {
    return u;
  }
  u = ParseProtoArg(op, "global_shift", param_map, &gs);
  if (!u.ok()) {
    return u;
  }
  circuit->gates.push_back(create_f(time, q0, q1, exp * exp_s, gs));
  return Status::OK();
}

Status IGate(const Operation& op, const SymbolMap& param_map,
             const unsigned time, QsimCircuit* circuit) {
  return SingleConstantGate(op, param_map, &qsim::Cirq::I<float>::Create, time,
                            circuit);
}

Status I2Gate(const Operation& op, const SymbolMap& param_map,
              const unsigned time, QsimCircuit* circuit) {
  return TwoConstantGate(op, param_map, &qsim::Cirq::I2<float>::Create, time,
                         circuit);
}

Status HGate(const Operation& op, const SymbolMap& param_map,
             const unsigned time, QsimCircuit* circuit) {
  return SingleEigenGate(op, param_map, &qsim::Cirq::HPowGate<float>::Create,
                         time, circuit);
}

Status XGate(const Operation& op, const SymbolMap& param_map,
             const unsigned time, QsimCircuit* circuit) {
  return SingleEigenGate(op, param_map, &qsim::Cirq::XPowGate<float>::Create,
                         time, circuit);
}

Status XXGate(const Operation& op, const SymbolMap& param_map,
              const unsigned time, QsimCircuit* circuit) {
  return TwoEigenGate(op, param_map, &qsim::Cirq::XXPowGate<float>::Create,
                      time, circuit);
}

Status YGate(const Operation& op, const SymbolMap& param_map,
             const unsigned time, QsimCircuit* circuit) {
  return SingleEigenGate(op, param_map, &qsim::Cirq::YPowGate<float>::Create,
                         time, circuit);
}

Status YYGate(const Operation& op, const SymbolMap& param_map,
              const unsigned time, QsimCircuit* circuit) {
  return TwoEigenGate(op, param_map, &qsim::Cirq::YYPowGate<float>::Create,
                      time, circuit);
}

Status ZGate(const Operation& op, const SymbolMap& param_map,
             const unsigned time, QsimCircuit* circuit) {
  return SingleEigenGate(op, param_map, &qsim::Cirq::ZPowGate<float>::Create,
                         time, circuit);
}

Status ZZGate(const Operation& op, const SymbolMap& param_map,
              const unsigned time, QsimCircuit* circuit) {
  return TwoEigenGate(op, param_map, &qsim::Cirq::ZZPowGate<float>::Create,
                      time, circuit);
}

Status CZGate(const Operation& op, const SymbolMap& param_map,
              const unsigned time, QsimCircuit* circuit) {
  return TwoEigenGate(op, param_map, &qsim::Cirq::CZPowGate<float>::Create,
                      time, circuit);
}

Status CXGate(const Operation& op, const SymbolMap& param_map,
              const unsigned time, QsimCircuit* circuit) {
  return TwoEigenGate(op, param_map, &qsim::Cirq::CXPowGate<float>::Create,
                      time, circuit);
}

Status SwapGate(const Operation& op, const SymbolMap& param_map,
                const unsigned time, QsimCircuit* circuit) {
  return TwoEigenGate(op, param_map, &qsim::Cirq::SwapPowGate<float>::Create,
                      time, circuit);
}

Status ISwapGate(const Operation& op, const SymbolMap& param_map,
                 const unsigned time, QsimCircuit* circuit) {
  return TwoEigenGate(op, param_map, &qsim::Cirq::ISwapPowGate<float>::Create,
                      time, circuit);
}

// single qubit PhasedXPow -> Create(time, q0, pexp, exp, gs)
inline Status PhasedXGate(const Operation& op, const SymbolMap& param_map,
                          const unsigned time, QsimCircuit* circuit) {
  int q0;
  bool unused;
  float pexp, pexp_s, exp, exp_s, gs;
  Status u;
  unused = absl::SimpleAtoi(op.qubits(0).id(), &q0);

  u = ParseProtoArg(op, "exponent", param_map, &exp);
  if (!u.ok()) {
    return u;
  }
  u = ParseProtoArg(op, "exponent_scalar", param_map, &exp_s);
  if (!u.ok()) {
    return u;
  }
  u = ParseProtoArg(op, "phase_exponent", param_map, &pexp);
  if (!u.ok()) {
    return u;
  }
  u = ParseProtoArg(op, "phase_exponent_scalar", param_map, &pexp_s);
  if (!u.ok()) {
    return u;
  }
  u = ParseProtoArg(op, "global_shift", param_map, &gs);
  if (!u.ok()) {
    return u;
  }
  circuit->gates.push_back(qsim::Cirq::PhasedXPowGate<float>::Create(
      time, q0, pexp * pexp_s, exp * exp_s, gs));
  return Status::OK();
}

// two qubit fsim -> Create(time, q0, q1, theta, phi)
inline Status FsimGate(const Operation& op, const SymbolMap& param_map,
                       const unsigned time, QsimCircuit* circuit) {
  int q0, q1;
  bool unused;
  float theta, theta_s, phi, phi_s;
  Status u;
  unused = absl::SimpleAtoi(op.qubits(0).id(), &q0);
  unused = absl::SimpleAtoi(op.qubits(1).id(), &q1);
  u = ParseProtoArg(op, "theta", param_map, &theta);
  if (!u.ok()) {
    return u;
  }
  u = ParseProtoArg(op, "theta_scalar", param_map, &theta_s);
  if (!u.ok()) {
    return u;
  }
  u = ParseProtoArg(op, "phi", param_map, &phi);
  if (!u.ok()) {
    return u;
  }
  u = ParseProtoArg(op, "phi_scalar", param_map, &phi_s);
  if (!u.ok()) {
    return u;
  }
  circuit->gates.push_back(qsim::Cirq::FSimGate<float>::Create(
      time, q0, q1, theta * theta_s, phi * phi_s));
  return Status::OK();
}

// two qubit phase iswap -> Create(time, q0, q1, pexp, exp)
inline Status PhasedISwapGate(const Operation& op, const SymbolMap& param_map,
                              const unsigned time, QsimCircuit* circuit) {
  int q0, q1;
  bool unused;
  float pexp, pexp_s, exp, exp_s;
  Status u;
  unused = absl::SimpleAtoi(op.qubits(0).id(), &q0);
  unused = absl::SimpleAtoi(op.qubits(1).id(), &q1);

  u = ParseProtoArg(op, "exponent", param_map, &exp);
  if (!u.ok()) {
    return u;
  }
  u = ParseProtoArg(op, "exponent_scalar", param_map, &exp_s);
  if (!u.ok()) {
    return u;
  }
  u = ParseProtoArg(op, "phase_exponent", param_map, &pexp);
  if (!u.ok()) {
    return u;
  }
  u = ParseProtoArg(op, "phase_exponent_scalar", param_map, &pexp_s);
  if (!u.ok()) {
    return u;
  }
  circuit->gates.push_back(qsim::Cirq::PhasedISwapPowGate<float>::Create(
      time, q0, q1, pexp * pexp_s, exp * exp_s));
  return Status::OK();
}

tensorflow::Status ParseAppendGate(const Operation& op,
                                   const SymbolMap& param_map,
                                   const unsigned time, QsimCircuit* circuit) {
  // map gate name -> callable to build that qsim gate from operation proto.
  static const absl::flat_hash_map<
      std::string, std::function<Status(const Operation&, const SymbolMap&,
                                        const unsigned, QsimCircuit*)>>
      func_map = {{"I", &IGate},       {"HP", &HGate},
                  {"XP", &XGate},      {"XXP", &XXGate},
                  {"YP", &YGate},      {"YYP", &YYGate},
                  {"ZP", &ZGate},      {"ZZP", &ZZGate},
                  {"CZP", &CZGate},    {"I2", &I2Gate},
                  {"CNP", &CXGate},    {"SP", &SwapGate},
                  {"ISP", &ISwapGate}, {"PXP", &PhasedXGate},
                  {"FSIM", &FsimGate}, {"PISP", &PhasedISwapGate}};

  auto build_f = func_map.find(op.gate().id());
  if (build_f == func_map.end()) {
    return Status(tensorflow::error::INVALID_ARGUMENT,
                  "Could not parse gate id: " + op.gate().id());
  }
  return build_f->second(op, param_map, time, circuit);
}

}  // namespace

tensorflow::Status QsimCircuitFromProgram(
    const Program& program, const SymbolMap& param_map, const int num_qubits,
    QsimCircuit* circuit,
    std::vector<qsim::GateFused<QsimGate>>* fused_circuit) {
  // Convert proto to qsim internal representation.
  circuit->num_qubits = num_qubits;
  int time = 0;
  for (const Moment& moment : program.circuit().moments()) {
    for (const Operation& op : moment.operations()) {
      Status status = ParseAppendGate(op, param_map, time, circuit);
      if (!status.ok()) {
        return status;
      }
    }
    time++;
  }

  // Build fused circuit.
  *fused_circuit = qsim::BasicGateFuser<QsimGate>().FuseGates(
      circuit->num_qubits, circuit->gates, time);
  return Status::OK();
}

}  // namespace tfq
