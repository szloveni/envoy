// Upgrade a single Envoy C++ file to the latest API version.
//
// Currently this tool is a WIP and only does inference of .pb[.validate].h
// #include locations. This already exercises some of the muscles we need, such
// as AST matching, rudimentary type inference and API type database lookup.
//
// NOLINT(namespace-envoy)

#include <fstream>
#include <iostream>
#include <regex>
#include <set>

// Declares clang::SyntaxOnlyAction.
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/ReplacementsYaml.h"

// Declares llvm::cl::extrahelp.
#include "llvm/Support/CommandLine.h"

#include "proto_cxx_utils.h"

#include "tools/type_whisperer/api_type_db.h"

#include "absl/strings/str_cat.h"

// Enable to see debug log messages.
#ifdef ENABLE_DEBUG_LOG
#define DEBUG_LOG(s)                                                                               \
  do {                                                                                             \
    std::cerr << (s) << std::endl;                                                                 \
  } while (0)
#else
#define DEBUG_LOG(s)
#endif

using namespace Envoy::Tools::TypeWhisperer;

namespace ApiBooster {

class ApiBooster : public clang::ast_matchers::MatchFinder::MatchCallback,
                   public clang::tooling::SourceFileCallbacks {
public:
  ApiBooster(std::map<std::string, clang::tooling::Replacements>& replacements)
      : replacements_(replacements) {}

  // AST match callback dispatcher.
  void run(const clang::ast_matchers::MatchFinder::MatchResult& match_result) override {
    clang::SourceManager& source_manager = match_result.Context->getSourceManager();
    DEBUG_LOG("AST match callback dispatcher");
    for (const auto it : match_result.Nodes.getMap()) {
      DEBUG_LOG(absl::StrCat("  Result for ", it.first, " [",
                             getSourceText(it.second.getSourceRange(), source_manager), "]"));
    }
    if (const auto* type_loc = match_result.Nodes.getNodeAs<clang::TypeLoc>("type")) {
      onTypeLocMatch(*type_loc, source_manager);
      return;
    }
    if (const auto* using_decl = match_result.Nodes.getNodeAs<clang::UsingDecl>("using_decl")) {
      onUsingDeclMatch(*using_decl, source_manager);
      return;
    }
    if (const auto* decl_ref_expr =
            match_result.Nodes.getNodeAs<clang::DeclRefExpr>("decl_ref_expr")) {
      onDeclRefExprMatch(*decl_ref_expr, source_manager);
      return;
    }
    if (const auto* call_expr = match_result.Nodes.getNodeAs<clang::CallExpr>("call_expr")) {
      onCallExprMatch(*call_expr, source_manager);
      return;
    }
    if (const auto* member_call_expr =
            match_result.Nodes.getNodeAs<clang::CXXMemberCallExpr>("member_call_expr")) {
      onMemberCallExprMatch(*member_call_expr, source_manager);
      return;
    }
    if (const auto* tmpl =
            match_result.Nodes.getNodeAs<clang::ClassTemplateSpecializationDecl>("tmpl")) {
      onClassTemplateSpecializationDeclMatch(*tmpl, source_manager);
      return;
    }
  }

  // Visitor callback for start of a compilation unit.
  bool handleBeginSource(clang::CompilerInstance& CI) override {
    source_api_proto_paths_.clear();
    return true;
  }

  // Visitor callback for end of a compilation unit.
  void handleEndSource() override {
    // Dump known API header paths to stdout for api_boost.py to rewrite with
    // (no rewriting support in this tool yet).
    for (const std::string& proto_path : source_api_proto_paths_) {
      std::cout << proto_path << std::endl;
    }
  }

private:
  // Match callback for TypeLoc. These are explicit mentions of the type in the
  // source. If we have a match on type, we should track the corresponding .pb.h
  // and attempt to upgrade.
  void onTypeLocMatch(const clang::TypeLoc& type_loc, const clang::SourceManager& source_manager) {
    const std::string type_name =
        type_loc.getType().getCanonicalType().getUnqualifiedType().getAsString();
    // Remove qualifiers, e.g. const.
    const clang::UnqualTypeLoc unqual_type_loc = type_loc.getUnqualifiedLoc();

    // Today we are only smart enought to rewrite ElaborateTypeLoc, which are
    // full namespace prefixed types. We probably will need to support more, in
    // particular if we want message-level type renaming. TODO(htuch): add more
    // supported AST TypeLoc classes as needed.
    const bool rewrite = unqual_type_loc.getTypeLocClass() == clang::TypeLoc::Elaborated;
    tryBoostType(unqual_type_loc.getType().getCanonicalType().getAsString(),
                 rewrite ? absl::make_optional<clang::SourceRange>(unqual_type_loc.getSourceRange())
                         : absl::nullopt,
                 source_manager, type_loc.getType()->getTypeClassName(), false);
  }

  // Match callback for clang::UsingDecl. These are 'using' aliases for API type
  // names.
  void onUsingDeclMatch(const clang::UsingDecl& using_decl,
                        const clang::SourceManager& source_manager) {
    // Not all using declaration are types, but we try the rewrite in case there
    // is such an API type database match.
    const clang::SourceRange source_range = clang::SourceRange(
        using_decl.getQualifierLoc().getBeginLoc(), using_decl.getNameInfo().getEndLoc());
    const std::string type_name = getSourceText(source_range, source_manager);
    tryBoostType(type_name, source_range, source_manager, "UsingDecl", true);
  }

  // Match callback for clang::DeclRefExpr. These occur when enums constants,
  // e.g. foo::bar::kBaz, appear in the source.
  void onDeclRefExprMatch(const clang::DeclRefExpr& decl_ref_expr,
                          const clang::SourceManager& source_manager) {
    // We don't need to consider non-namespace qualified DeclRefExprfor now (no
    // renaming support yet).
    if (!decl_ref_expr.hasQualifier()) {
      return;
    }
    const std::string decl_name = decl_ref_expr.getNameInfo().getAsString();
    // There are generated methods to stringify/parse/validate enum values,
    // these need special treatment as they look like types with special
    // suffices.
    for (const std::string& enum_generated_method_suffix : {"_Name", "_Parse", "_IsValid"}) {
      if (absl::EndsWith(decl_name, enum_generated_method_suffix)) {
        // Remove trailing suffix from reference for replacement range and type
        // name purposes.
        const clang::SourceLocation begin_loc = decl_ref_expr.getBeginLoc();
        const std::string type_name_with_suffix =
            getSourceText(decl_ref_expr.getSourceRange(), source_manager);
        const std::string type_name = type_name_with_suffix.substr(
            0, type_name_with_suffix.size() - enum_generated_method_suffix.size());
        tryBoostType(type_name, begin_loc, type_name.size(), source_manager,
                     "DeclRefExpr suffixed " + enum_generated_method_suffix, false);
        return;
      }
    }
    // Remove trailing : from namespace qualifier.
    const clang::SourceRange source_range =
        clang::SourceRange(decl_ref_expr.getQualifierLoc().getBeginLoc(),
                           decl_ref_expr.getQualifierLoc().getEndLoc().getLocWithOffset(-1));
    // Generally we pull the type from the named entity's declaration type,
    // since this allows us to map from things like envoy::type::HTTP2 to the
    // underlying fully qualified envoy::type::CodecClientType::HTTP2 prior to
    // API type database lookup. However, for the generated static methods, we
    // don't want to deal with lookup via the function type, so we do the fixup
    // here.
    const std::set<std::string> ProtoStaticGeneratedMethod = {
        "descriptor",
        "default_instance",
    };
    const bool is_proto_static_generated_method = ProtoStaticGeneratedMethod.count(decl_name) != 0;
    const std::string type_name = is_proto_static_generated_method
                                      ? getSourceText(source_range, source_manager)
                                      : decl_ref_expr.getDecl()
                                            ->getType()
                                            .getCanonicalType()
                                            .getUnqualifiedType()
                                            .getAsString();
    tryBoostType(type_name, source_range, source_manager, "DeclRefExpr", true);
  }

  // Match callback clang::CallExpr. We don't need to rewrite, but if it's something like
  // loadFromYamlAndValidate, we might need to look at the argument type to
  // figure out any corresponding .pb.validate.h we require.
  void onCallExprMatch(const clang::CallExpr& call_expr,
                       const clang::SourceManager& source_manager) {
    auto* direct_callee = call_expr.getDirectCallee();
    if (direct_callee != nullptr) {
      const std::unordered_map<std::string, int> ValidateNameToArg = {
          {"loadFromYamlAndValidate", 1},
          {"loadFromFileAndValidate", 1},
          {"downcastAndValidate", 0},
          {"validate", 0},
      };
      const std::string& callee_name = direct_callee->getNameInfo().getName().getAsString();
      const auto arg = ValidateNameToArg.find(callee_name);
      // Sometimes we hit false positives because we aren't qualifying above.
      // TODO(htuch): fix this.
      if (arg != ValidateNameToArg.end() && arg->second < call_expr.getNumArgs()) {
        const std::string type_name = call_expr.getArg(arg->second)
                                          ->getType()
                                          .getCanonicalType()
                                          .getUnqualifiedType()
                                          .getAsString();
        tryBoostType(type_name, {}, source_manager, "validation invocation", true, true);
      }
    }
  }

  // Match callback clang::CallMemberExpr. We rewrite things like
  // ->mutable_foo() to ->mutable_foo_new_name() during renames.
  void onMemberCallExprMatch(const clang::CXXMemberCallExpr& member_call_expr,
                             const clang::SourceManager& source_manager) {
    const std::string type_name =
        member_call_expr.getObjectType().getCanonicalType().getUnqualifiedType().getAsString();
    const auto latest_type_info = getLatestTypeInformationFromCType(type_name);
    // If this isn't a known API type, our work here is done.
    if (!latest_type_info) {
      return;
    }
    const clang::SourceRange source_range = {member_call_expr.getExprLoc(),
                                             member_call_expr.getExprLoc()};
    const std::string method_name = getSourceText(source_range, source_manager);
    DEBUG_LOG(
        absl::StrCat("Matched member call expr on ", type_name, " with method ", method_name));
    const auto method_rename =
        ProtoCxxUtils::renameMethod(method_name, latest_type_info->field_renames_);
    if (method_rename) {
      const clang::tooling::Replacement method_replacement(
          source_manager, source_range.getBegin(), sourceRangeLength(source_range, source_manager),
          *method_rename);
      insertReplacement(method_replacement);
    }
  }

  // Match callback for clang::ClassTemplateSpecializationDecl. An additional
  // place we need to look for .pb.validate.h reference is instantiation of
  // FactoryBase.
  void onClassTemplateSpecializationDeclMatch(const clang::ClassTemplateSpecializationDecl& tmpl,
                                              const clang::SourceManager& source_manager) {
    const std::string tmpl_type_name = tmpl.getSpecializedTemplate()
                                           ->getInjectedClassNameSpecialization()
                                           .getCanonicalType()
                                           .getAsString();
    if (tmpl_type_name == "FactoryBase<type-parameter-0-0>") {
      const std::string type_name = tmpl.getTemplateArgs()
                                        .get(0)
                                        .getAsType()
                                        .getCanonicalType()
                                        .getUnqualifiedType()
                                        .getAsString();
      tryBoostType(type_name, {}, source_manager, "FactoryBase template", true, true);
    }
  }

  // Attempt to boost a given type and rewrite the given source range.
  void tryBoostType(const std::string& type_name, absl::optional<clang::SourceRange> source_range,
                    const clang::SourceManager& source_manager, absl::string_view debug_description,
                    bool requires_enum_truncation, bool validation_required = false) {
    if (source_range) {
      tryBoostType(type_name, source_manager.getSpellingLoc(source_range->getBegin()),
                   sourceRangeLength(*source_range, source_manager), source_manager,
                   debug_description, requires_enum_truncation, validation_required);
    } else {
      tryBoostType(type_name, {}, -1, source_manager, debug_description, requires_enum_truncation,
                   validation_required);
    }
  }

  void tryBoostType(const std::string& type_name, clang::SourceLocation begin_loc, int length,
                    const clang::SourceManager& source_manager, absl::string_view debug_description,
                    bool requires_enum_truncation, bool validation_required = false) {
    const auto latest_type_info = getLatestTypeInformationFromCType(type_name);
    // If this isn't a known API type, our work here is done.
    if (!latest_type_info) {
      return;
    }
    DEBUG_LOG(absl::StrCat("Matched type '", type_name, "' (", debug_description, ") length ",
                           length, " at ", begin_loc.printToString(source_manager)));
    // Track corresponding imports.
    source_api_proto_paths_.insert(adjustProtoSuffix(latest_type_info->proto_path_, ".pb.h"));
    if (validation_required) {
      source_api_proto_paths_.insert(
          adjustProtoSuffix(latest_type_info->proto_path_, ".pb.validate.h"));
    }
    // Not all AST matchers know how to do replacements (yet?).
    if (length == -1) {
      return;
    }
    // We need to look at the text we're replacing to decide whether we should
    // use the qualified C++'ified proto name.
    const bool qualified =
        getSourceText(begin_loc, length, source_manager).find("::") != std::string::npos;
    // Add corresponding replacement.
    const clang::tooling::Replacement type_replacement(
        source_manager, begin_loc, length,
        ProtoCxxUtils::protoToCxxType(latest_type_info->type_name_, qualified,
                                      latest_type_info->enum_type_ && requires_enum_truncation));
    insertReplacement(type_replacement);
  }

  void insertReplacement(const clang::tooling::Replacement& replacement) {
    llvm::Error error = replacements_[replacement.getFilePath()].add(replacement);
    if (error) {
      std::cerr << "  Replacement insertion error: " << llvm::toString(std::move(error))
                << std::endl;
    } else {
      std::cerr << "  Replacement added: " << replacement.toString() << std::endl;
    }
  }

  // Modeled after getRangeSize() in Clang's Replacements.cpp. Turns out it's
  // non-trivial to get the actual length of a SourceRange, as the end location
  // point to the start of the last token.
  int sourceRangeLength(clang::SourceRange source_range,
                        const clang::SourceManager& source_manager) {
    const clang::SourceLocation spelling_begin =
        source_manager.getSpellingLoc(source_range.getBegin());
    const clang::SourceLocation spelling_end = source_manager.getSpellingLoc(source_range.getEnd());
    std::pair<clang::FileID, unsigned> start = source_manager.getDecomposedLoc(spelling_begin);
    std::pair<clang::FileID, unsigned> end = source_manager.getDecomposedLoc(spelling_end);
    if (start.first != end.first) {
      return -1;
    }
    end.second += clang::Lexer::MeasureTokenLength(spelling_end, source_manager, lexer_lopt_);
    return end.second - start.second;
  }

  std::string getSourceText(clang::SourceLocation begin_loc, int size,
                            const clang::SourceManager& source_manager) {
    return clang::Lexer::getSourceText(
        {clang::SourceRange(begin_loc, begin_loc.getLocWithOffset(size)), false}, source_manager,
        lexer_lopt_, 0);
  }

  std::string getSourceText(clang::SourceRange source_range,
                            const clang::SourceManager& source_manager) {
    return clang::Lexer::getSourceText(clang::CharSourceRange::getTokenRange(source_range),
                                       source_manager, lexer_lopt_, 0);
  }

  void addNamedspaceQualifiedTypeReplacement() {}

  // Remove .proto from a path, apply specified suffix instead.
  std::string adjustProtoSuffix(absl::string_view proto_path, absl::string_view suffix) {
    return absl::StrCat(proto_path.substr(0, proto_path.size() - 6), suffix);
  }

  // Obtain the latest type information for a given from C++ type, e.g. envoy:config::v2::Cluster,
  // from the API type database.
  absl::optional<TypeInformation>
  getLatestTypeInformationFromCType(const std::string& c_type_name) {
    // Ignore compound or non-API types.
    // TODO(htuch): this is all super hacky and not really right, we should be
    // removing qualifiers etc. to get to the underlying type name.
    const std::string type_name = std::regex_replace(c_type_name, std::regex("^(class|enum) "), "");
    if (!absl::StartsWith(type_name, "envoy::") || absl::StrContains(type_name, " ")) {
      return {};
    }
    const std::string proto_type_name = ProtoCxxUtils::cxxToProtoType(type_name);

    // Use API type database to map from proto type to path.
    auto result = ApiTypeDb::getLatestTypeInformation(proto_type_name);
    if (result) {
      // Remove the .proto extension.
      return result;
    } else if (!absl::StartsWith(proto_type_name, "envoy.HotRestart") &&
               !absl::StartsWith(proto_type_name, "envoy.RouterCheckToolSchema") &&
               !absl::StartsWith(proto_type_name, "envoy.test") &&
               !absl::StartsWith(proto_type_name, "envoy.tracers.xray.daemon")) {
      // Die hard if we don't have a useful proto type for something that looks
      // like an API type(modulo a short whitelist).
      std::cerr << "Unknown API type: " << proto_type_name << std::endl;
      // TODO(htuch): maybe there is a nicer way to terminate AST traversal?
      ::exit(1);
    }

    return {};
  }

  // Set of inferred .pb[.validate].h, updated as the AST matcher callbacks above fire.
  std::set<std::string> source_api_proto_paths_;
  // Map from source file to replacements.
  std::map<std::string, clang::tooling::Replacements>& replacements_;
  // Language options for interacting with Lexer. Currently empty.
  clang::LangOptions lexer_lopt_;
}; // namespace ApiBooster

} // namespace ApiBooster

int main(int argc, const char** argv) {
  // Apply a custom category to all command-line options so that they are the
  // only ones displayed.
  llvm::cl::OptionCategory api_booster_tool_category("api-booster options");

  clang::tooling::CommonOptionsParser options_parser(argc, argv, api_booster_tool_category);
  clang::tooling::RefactoringTool tool(options_parser.getCompilations(),
                                       options_parser.getSourcePathList());

  ApiBooster::ApiBooster api_booster(tool.getReplacements());
  clang::ast_matchers::MatchFinder finder;

  // Match on all mentions of types in the AST.
  auto type_matcher =
      clang::ast_matchers::typeLoc(clang::ast_matchers::isExpansionInMainFile()).bind("type");
  finder.addMatcher(type_matcher, &api_booster);

  // Match on all "using" declarations.
  auto using_decl_matcher =
      clang::ast_matchers::usingDecl(clang::ast_matchers::isExpansionInMainFile())
          .bind("using_decl");
  finder.addMatcher(using_decl_matcher, &api_booster);

  // Match on references to enum constants.
  auto decl_ref_expr_matcher =
      clang::ast_matchers::declRefExpr(clang::ast_matchers::isExpansionInMainFile())
          .bind("decl_ref_expr");
  finder.addMatcher(decl_ref_expr_matcher, &api_booster);

  // Match on all call expressions. We are interested in particular in calls
  // where validation on protos is performed.
  auto call_matcher =
      clang::ast_matchers::callExpr(clang::ast_matchers::isExpansionInMainFile()).bind("call_expr");
  finder.addMatcher(call_matcher, &api_booster);

  // Match on all .foo() or ->foo() expressions. We are interested in these for renames
  // and deprecations.
  auto member_call_expr =
      clang::ast_matchers::cxxMemberCallExpr(clang::ast_matchers::isExpansionInMainFile())
          .bind("member_call_expr");
  finder.addMatcher(member_call_expr, &api_booster);

  // Match on all template instantiations.We are interested in particular in
  // instantiations of factories where validation on protos is performed.
  auto tmpl_matcher = clang::ast_matchers::classTemplateSpecializationDecl(
                          clang::ast_matchers::isExpansionInMainFile())
                          .bind("tmpl");
  finder.addMatcher(tmpl_matcher, &api_booster);

  // Apply ApiBooster to AST matches. This will generate a set of replacements in
  // tool.getReplacements().
  const int run_result = tool.run(newFrontendActionFactory(&finder, &api_booster).get());
  if (run_result != 0) {
    std::cerr << "Exiting with non-zero result " << run_result << std::endl;
    return run_result;
  }

  // Serialize replacements to <main source file path>.clang-replacements.yaml.
  // These are suitable for consuming by clang-apply-replacements.
  for (const auto& file_replacement : tool.getReplacements()) {
    // Populate TranslationUnitReplacements from file replacements (this is what
    // there exists llvm::yaml serialization support for).
    clang::tooling::TranslationUnitReplacements tu_replacements;
    tu_replacements.MainSourceFile = file_replacement.first;
    for (const auto& r : file_replacement.second) {
      tu_replacements.Replacements.push_back(r);
      DEBUG_LOG(r.toString());
    }
    // Serialize TranslationUnitReplacements to YAML.
    std::string yaml_content;
    llvm::raw_string_ostream yaml_content_stream(yaml_content);
    llvm::yaml::Output yaml(yaml_content_stream);
    yaml << tu_replacements;
    // Write to <main source file path>.clang-replacements.yaml.
    std::ofstream serialized_replacement_file(tu_replacements.MainSourceFile +
                                              ".clang-replacements.yaml");
    serialized_replacement_file << yaml_content_stream.str();
  }

  return 0;
}
