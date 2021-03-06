/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "IRAssembler.h"

#include <boost/functional/hash.hpp>
#include <sstream>
#include <string>

namespace {

#define OP(OP, CODE, FORMAT, KIND, STR) {OPCODE_##OP, STR},
std::unordered_map<DexOpcode, std::string, boost::hash<DexOpcode>>
    opcode_to_string_table = {
        OPS
        {IOPCODE_LOAD_PARAM, "load-param"},
        {IOPCODE_LOAD_PARAM_OBJECT, "load-param-object"},
        {IOPCODE_LOAD_PARAM_WIDE, "load-param-wide"},
        {IOPCODE_MOVE_RESULT_PSEUDO, "move-result-pseudo"},
        {IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, "move-result-pseudo-object"},
        {IOPCODE_MOVE_RESULT_PSEUDO_WIDE, "move-result-pseudo-wide"},
};
#undef OP

#define OP(OP, CODE, FORMAT, KIND, STR) {STR, OPCODE_##OP},
std::unordered_map<std::string, DexOpcode> string_to_opcode_table = {
    OPS
    {"load-param", IOPCODE_LOAD_PARAM},
    {"load-param-object", IOPCODE_LOAD_PARAM_OBJECT},
    {"load-param-wide", IOPCODE_LOAD_PARAM_WIDE},
    {"move-result-pseudo", IOPCODE_MOVE_RESULT_PSEUDO},
    {"move-result-pseudo-object", IOPCODE_MOVE_RESULT_PSEUDO_OBJECT},
    {"move-result-pseudo-wide", IOPCODE_MOVE_RESULT_PSEUDO_WIDE},
};
#undef OP

using LabelDefs = std::unordered_map<std::string, MethodItemEntry*>;
using LabelRefs = std::unordered_map<const IRInstruction*, std::string>;

uint16_t reg_from_str(const std::string& reg_str) {
  always_assert(reg_str.at(0) == 'v');
  uint16_t reg;
  sscanf(&reg_str.c_str()[1], "%hu", &reg);
  return reg;
}

std::string reg_to_str(uint16_t reg) {
  return "v" + std::to_string(reg);
}

s_expr to_s_expr(const IRInstruction* insn,
                 const std::unordered_map<const IRInstruction*, std::string>&
                     insn_to_label) {
  auto op = insn->opcode();
  auto opcode_str = opcode_to_string_table.at(op);
  std::vector<s_expr> s_exprs{s_expr(opcode_str)};
  if (insn->dests_size()) {
    s_exprs.emplace_back(reg_to_str(insn->dest()));
  }
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    s_exprs.emplace_back(reg_to_str(insn->src(i)));
  }
  switch (opcode::ref(op)) {
    case opcode::Ref::None:
      break;
    case opcode::Ref::Field:
    case opcode::Ref::Method:
    case opcode::Ref::Data:
      always_assert_log(false, "Not yet supported");
      break;
    case opcode::Ref::String:
      s_exprs.emplace_back(insn->get_string()->str());
      break;
    case opcode::Ref::Literal:
      s_exprs.emplace_back(std::to_string(insn->get_literal()));
      break;
    case opcode::Ref::Type:
      s_exprs.emplace_back(insn->get_type()->get_name()->str());
      break;
  }
  if (opcode::has_offset(op)) {
    s_exprs.emplace_back(insn_to_label.at(insn));
  }
  return s_expr(s_exprs);
}

std::unique_ptr<IRInstruction> instruction_from_s_expr(
    const std::string& opcode_str, const s_expr& e, LabelRefs* label_refs) {
  auto op_it = string_to_opcode_table.find(opcode_str);
  always_assert_log(op_it != string_to_opcode_table.end(),
                    "'%s' is not a valid opcode",
                    opcode_str.c_str());
  auto op = op_it->second;
  auto insn = std::make_unique<IRInstruction>(op);
  std::string reg_str;
  s_expr tail = e;
  if (insn->dests_size()) {
    s_patn({s_patn(&reg_str)}, tail)
        .must_match(tail, "Expected dest reg for " + opcode_str);
    insn->set_dest(reg_from_str(reg_str));
  }
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    s_patn({s_patn(&reg_str)}, tail)
        .must_match(tail, "Expected src reg for" + opcode_str);
    insn->set_src(i, reg_from_str(reg_str));
  }
  switch (opcode::ref(op)) {
    case opcode::Ref::None:
      break;
    case opcode::Ref::Field:
    case opcode::Ref::Method:
    case opcode::Ref::Data:
      always_assert_log(false, "Not yet supported");
      break;
    case opcode::Ref::String: {
      std::string str;
      s_patn({s_patn(&str)}, tail)
          .must_match(tail, "Expecting string literal for " + opcode_str);
      auto* dex_str = DexString::make_string(str);
      insn->set_string(dex_str);
      break;
    }
    case opcode::Ref::Literal: {
      std::string num_str;
      s_patn({s_patn(&num_str)}, tail)
          .must_match(tail, "Expecting numeric literal for " + opcode_str);
      int64_t num;
      std::istringstream in(num_str);
      in >> num;
      insn->set_literal(num);
      break;
    }
    case opcode::Ref::Type: {
      std::string type_str;
      s_patn({s_patn(&type_str)}, tail)
          .must_match(tail, "Expecting type specifier for " + opcode_str);
      DexType* ty = DexType::make_type(type_str.c_str());
      insn->set_type(ty);
      break;
    }
  }
  if (opcode::has_offset(op)) {
    std::string label_str;
    s_patn({s_patn(&label_str)}, tail)
        .must_match(tail, "Expecting label for " + opcode_str);
    label_refs->emplace(insn.get(), label_str);
  }

  always_assert_log(tail.is_nil(),
                    "Found unexpected trailing items when parsing %s: %s",
                    opcode_str.c_str(),
                    tail.str().c_str());
  return insn;
}

/*
 * Connect label defs to label refs via creation of MFLOW_TARGET instances
 */
void handle_labels(IRCode* code,
                   const LabelDefs& label_defs,
                   const LabelRefs label_refs) {
  for (auto& mie : InstructionIterable(code)) {
    auto* insn = mie.insn;
    if (label_refs.count(insn)) {
      auto target_mie = label_defs.at(label_refs.at(insn));
      // Currently we can only jump once to a given label
      always_assert(target_mie->type == MFLOW_FALLTHROUGH);
      target_mie->type = MFLOW_TARGET;
      target_mie->target = new BranchTarget();
      target_mie->target->type = BRANCH_SIMPLE;
      target_mie->target->src = &mie;
    }
  }
}

} // namespace

namespace assembler {

s_expr to_s_expr(const IRCode* code) {
  std::vector<s_expr> insn_exprs;
  std::unordered_map<const IRInstruction*, std::string> insn_to_label;
  size_t label_ctr{0};
  auto generate_label_name = [&]() {
    return ":L" + std::to_string(label_ctr++);
  };
  // Gather jump targets and give them string names
  for (auto it = code->begin(); it != code->end(); ++it) {
    switch (it->type) {
      case MFLOW_TARGET: {
        auto bt = it->target;
        always_assert_log(bt->type == BRANCH_SIMPLE, "Not yet implemented");
        insn_to_label.emplace(bt->src->insn, generate_label_name());
        break;
      }
      case MFLOW_CATCH:
        always_assert_log(false, "Not yet implemented");
        break;
      default:
        break;
    }
  }
  // Now emit the exprs
  for (auto it = code->begin(); it != code->end(); ++it) {
    switch (it->type) {
      case MFLOW_OPCODE:
        insn_exprs.emplace_back(::to_s_expr(it->insn, insn_to_label));
        break;
      case MFLOW_TRY:
      case MFLOW_CATCH:
      case MFLOW_DEBUG:
      case MFLOW_POSITION:
        always_assert_log(false, "Not yet implemented");
        break;
      case MFLOW_TARGET:
      case MFLOW_FALLTHROUGH:
        break;
      case MFLOW_DEX_OPCODE:
        not_reached();
    }
  }
  return s_expr(insn_exprs);
}

std::unique_ptr<IRCode> ircode_from_s_expr(const s_expr& e) {
  s_expr insns_expr;
  auto code = std::make_unique<IRCode>();
  always_assert(s_patn({}, insns_expr).match_with(e));
  always_assert_log(insns_expr.size() > 0, "Empty instruction list?! %s");
  LabelDefs label_defs;
  LabelRefs label_refs;

  for (size_t i = 0; i < insns_expr.size(); ++i) {
    std::string keyword;
    s_expr tail;
    always_assert(s_patn({s_patn(&keyword)}, tail).match_with(insns_expr[i]));
    if (keyword[0] == ':') {
      auto label = keyword;
      always_assert_log(
          label_defs.count(label) == 0, "Duplicate label %s", label.c_str());
      // We insert a MFLOW_FALLTHROUGH that may become a MFLOW_TARGET if
      // something points to it
      auto maybe_target = new MethodItemEntry();
      label_defs.emplace(label, maybe_target);
      code->push_back(*maybe_target);
    } else {
      auto insn = instruction_from_s_expr(keyword, tail, &label_refs);
      always_assert(insn != nullptr);
      code->push_back(insn.release());
    }
  }
  handle_labels(code.get(), label_defs, label_refs);

  return code;
}

std::unique_ptr<IRCode> ircode_from_string(const std::string& s) {
  std::istringstream input(s);
  s_expr_istream s_expr_input(input);
  s_expr expr;
  while (s_expr_input.good()) {
    s_expr_input >> expr;
    if (s_expr_input.eoi()) {
      break;
    }
  }
  return ircode_from_s_expr(expr);
}

} // assembler
