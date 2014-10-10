//===-- ManifestLoader.cpp ------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llbuild/Ninja/ManifestLoader.h"

#include "llbuild/Ninja/Lexer.h"
#include "llbuild/Ninja/Parser.h"

#include <cstdlib>
#include <sstream>
#include <vector>

using namespace llbuild;
using namespace llbuild::ninja;

#pragma mark - ManifestLoaderActions

ManifestLoaderActions::~ManifestLoaderActions() {
}

#pragma mark - ManifestLoader Implementation

namespace {

/// Manifest loader implementation.
///
/// For simplicity, we just directly implement the parser actions interface.
class ManifestLoaderImpl: public ParseActions {
  struct IncludeEntry {
    /// The file that is being processed.
    std::string Filename;
    /// An owning reference to the data consumed by the parser.
    std::unique_ptr<char[]> Data;
    /// The parser for the file.
    std::unique_ptr<Parser> Parser;
    /// The active binding set.
    BindingSet &Bindings;

    IncludeEntry(const std::string& Filename,
                 std::unique_ptr<char[]> Data,
                 std::unique_ptr<class Parser> Parser,
                 BindingSet &Bindings)
      : Filename(Filename), Data(std::move(Data)), Parser(std::move(Parser)),
        Bindings(Bindings) {}
  };

  std::string MainFilename;
  ManifestLoaderActions &Actions;
  std::unique_ptr<Manifest> TheManifest;
  std::vector<IncludeEntry> IncludeStack;

public:
  ManifestLoaderImpl(std::string MainFilename, ManifestLoaderActions &Actions)
    : MainFilename(MainFilename), Actions(Actions), TheManifest(nullptr)
  {
  }

  std::unique_ptr<Manifest> load() {
    // Create the manifest.
    TheManifest.reset(new Manifest);

    // Enter the main file.
    if (!enterFile(MainFilename, TheManifest->getBindings()))
      return nullptr;

    // Run the parser.
    assert(IncludeStack.size() == 1);
    getCurrentParser()->parse();
    assert(IncludeStack.size() == 0);

    return std::move(TheManifest);
  }

  bool enterFile(const std::string& Filename, BindingSet& Bindings,
                 const Token* ForToken = nullptr) {
    // Load the file data.
    std::unique_ptr<char[]> Data;
    uint64_t Length;
    std::string FromFilename = IncludeStack.empty() ? Filename :
      getCurrentFilename();
    if (!Actions.readFileContents(FromFilename, Filename, ForToken, &Data,
                                  &Length))
      return false;

    // Push a new entry onto the include stack.
    std::unique_ptr<Parser> FileParser(new Parser(Data.get(), Length, *this));
    IncludeStack.push_back(IncludeEntry(Filename, std::move(Data),
                                        std::move(FileParser),
                                        Bindings));

    return true;
  }

  void exitCurrentFile() {
    IncludeStack.pop_back();
  }

  ManifestLoaderActions& getActions() { return Actions; }
  Parser* getCurrentParser() const {
    assert(!IncludeStack.empty());
    return IncludeStack.back().Parser.get();
  }
  const std::string& getCurrentFilename() const {
    assert(!IncludeStack.empty());
    return IncludeStack.back().Filename;
  }
  BindingSet& getCurrentBindings() const {
    assert(!IncludeStack.empty());
    return IncludeStack.back().Bindings;
  }

  /// Given a string template token, evaluate it against the given \arg Bindings
  /// and return the resulting string.
  std::string evalString(const Token& Value, const BindingSet& Bindings) {
    assert(Value.TokenKind == Token::Kind::String && "invalid token kind");

    // Scan the string for escape sequences or variable references, accumulating
    // output pieces as we go.
    //
    // FIXME: Rewrite this with StringRef once we have it, and make efficient.
    std::stringstream Result;
    const char* Start = Value.Start;
    const char* End = Value.Start + Value.Length;
    const char* Pos = Start;
    while (Pos != End) {
      // Find the next '$'.
      const char* PieceStart = Pos;
      for (; Pos != End; ++Pos) {
        if (*Pos == '$')
          break;
      }

      // Add the current piece, if non-empty.
      if (Pos != PieceStart)
        Result << std::string(PieceStart, Pos);

      // If we are at the end, we are done.
      if (Pos == End)
        break;

      // Otherwise, we have a '$' character to handle.
      ++Pos;
      if (Pos == End) {
        error("invalid '$'-escape at end of string", Value);
        break;
      }

      // If this is a newline continuation, skip it and all leading space.
      char Char = *Pos;
      if (Char == '\n') {
        ++Pos;
        while (Pos != End && isspace(*Pos))
          ++Pos;
        continue;
      }

      // If this is single character escape, honor it.
      if (Char == ' ' || Char == ':' || Char == '$') {
        Result << Char;
        ++Pos;
        continue;
      }

      // If this is a braced variable reference, expand it.
      if (Char == '{') {
        // Scan until the end of the reference, checking validity of the
        // identifier name as we go.
        ++Pos;
        const char* VarStart = Pos;
        bool IsValid = true;
        while (true) {
          // If we reached the end of the string, this is an error.
          if (Pos == End) {
            error("invalid variable reference in string (missing trailing '}')",
                  Value);
            break;
          }

          // If we found the end of the reference, resolve it.
          char Char = *Pos;
          if (Char == '}') {
            // If this identifier isn't valid, emit an error.
            if (!IsValid) {
              error("invalid variable name in reference", Value);
            } else {
              Result << Bindings.lookup(std::string(VarStart, Pos - VarStart));
            }
            ++Pos;
            break;
          }

          // Track whether this is a valid identifier.
          if (!Lexer::isIdentifierChar(Char))
            IsValid = false;

          ++Pos;
        }
        continue;
      }

      // If this is a simple variable reference, expand it.
      if (Lexer::isSimpleIdentifierChar(Char)) {
        const char* VarStart = Pos;
        // Scan until the end of the simple identifier.
        ++Pos;
        while (Pos != End && Lexer::isSimpleIdentifierChar(*Pos))
          ++Pos;
        Result << Bindings.lookup(std::string(VarStart, Pos-VarStart));
        continue;
      }

      // Otherwise, we have an invalid '$' escape.
      error("invalid '$'-escape (literal '$' should be written as '$$')",
            Value);
      break;
    }

    return Result.str();
  }

  /// @name Parse Actions Interfaces
  /// @{

  virtual void initialize(ninja::Parser *Parser) override { }

  virtual void error(std::string Message, const Token &At) override {
    Actions.error(getCurrentFilename(), Message, At);
  }

  virtual void actOnBeginManifest(std::string Name) override { }

  virtual void actOnEndManifest() override {
    exitCurrentFile();
  }

  virtual void actOnBindingDecl(const Token& NameTok,
                                const Token& ValueTok) override {
    // Extract the name string.
    std::string Name(NameTok.Start, NameTok.Length);

    // Evaluate the value string with the current top-level bindings.
    std::string Value(evalString(ValueTok, getCurrentBindings()));

    getCurrentBindings().insert(Name, Value);
  }

  virtual void actOnDefaultDecl(const std::vector<Token>& NameToks) override {
    // Resolve all of the inputs and outputs.
    for (auto& NameTok: NameToks) {
      std::string Name(NameTok.Start, NameTok.Length);

      auto it = TheManifest->getNodes().find(Name);
      if (it == TheManifest->getNodes().end()) {
        error("unknown target name", NameTok);
        continue;
      }

      TheManifest->getDefaultTargets().push_back(it->second.get());
    }
  }

  virtual void actOnIncludeDecl(bool IsInclude,
                                const Token& PathTok) override {
    std::string Path = evalString(PathTok, getCurrentBindings());

    // Enter the new file, with a new binding scope if this is a "subninja"
    // decl.
    if (IsInclude) {
      if (enterFile(Path, getCurrentBindings(), &PathTok)) {
        // Run the parser for the included file.
        getCurrentParser()->parse();
      }
    } else {
      // Establish a local binding set and use that to contain the bindings for
      // the subninja.
      //
      // FIXME: This binding set will disappear once the parsing is done, so we
      // can never store a reference to it. That will be a problem if we want to
      // postpone build decl binding expansion until after parsing (by giving
      // the rule a reference to its binding set). However we might end up just
      // binding up front so holding off on extending the lifetime of this until
      // then...
      BindingSet SubninjaBindings(&getCurrentBindings());
      if (enterFile(Path, SubninjaBindings, &PathTok)) {
        // Run the parser for the included file.
        getCurrentParser()->parse();
      }
    }
  }

  virtual BuildResult
  actOnBeginBuildDecl(const Token& NameTok,
                      const std::vector<Token> &OutputTokens,
                      const std::vector<Token> &InputTokens,
                      unsigned NumExplicitInputs,
                      unsigned NumImplicitInputs) override {
    std::string Name(NameTok.Start, NameTok.Length);

    // Resolve the rule.
    auto it = TheManifest->getRules().find(Name);
    Rule *Rule;
    if (it == TheManifest->getRules().end()) {
      error("unknown rule", NameTok);

      // Ensure we always have a rule for each command.
      Rule = TheManifest->getPhonyRule();
    } else {
      Rule = it->second.get();
    }

    // Resolve all of the inputs and outputs.
    std::vector<Node*> Outputs;
    std::vector<Node*> Inputs;
    for (auto& Token: OutputTokens) {
      // Evaluate the token string.
      std::string Path = evalString(Token, getCurrentBindings());
      if (Path.empty()) {
        error("empty output path", Token);
      }
      Outputs.push_back(TheManifest->getOrCreateNode(Path));
    }
    for (auto& Token: InputTokens) {
      // Evaluate the token string.
      std::string Path = evalString(Token, getCurrentBindings());
      if (Path.empty()) {
        error("empty input path", Token);
      }
      Inputs.push_back(TheManifest->getOrCreateNode(Path));
    }

    Command *Decl = new Command(Rule, Outputs, Inputs, NumExplicitInputs,
                                NumImplicitInputs);
    TheManifest->getCommands().push_back(std::unique_ptr<Command>(Decl));

    return Decl;
  }

  virtual void actOnBuildBindingDecl(BuildResult AbstractDecl,
                                     const Token& NameTok,
                                     const Token& ValueTok) override {
    Command* Decl = static_cast<Command*>(AbstractDecl);

    std::string Name(NameTok.Start, NameTok.Length);

    // FIXME: It probably should be an error to assign to the same parameter
    // multiple times, but Ninja doesn't diagnose this.

    // The value in a build decl is always evaluated immediately, but only in
    // the context of the top-level bindings.
    //
    // FIXME: This needs to be the inherited bindings, once we support includes.
    Decl->getParameters()[Name] = evalString(ValueTok,
                                             getCurrentBindings());
  }

  virtual void actOnEndBuildDecl(PoolResult Decl,
                                const Token& StartTok) override { }

  virtual PoolResult actOnBeginPoolDecl(const Token& NameTok) override {
    std::string Name(NameTok.Start, NameTok.Length);

    // Find the hash slot.
    auto& Result = TheManifest->getPools()[Name];

    // Diagnose if the pool already exists (we still create a new one).
    if (Result.get()) {
      // The pool already exists.
      error("duplicate pool", NameTok);
    }

    // Insert the new pool.
    Pool* Decl = new Pool(Name);
    Result.reset(Decl);
    return static_cast<PoolResult>(Decl);
  }

  virtual void actOnPoolBindingDecl(PoolResult AbstractDecl,
                                    const Token& NameTok,
                                    const Token& ValueTok) override {
    Pool* Decl = static_cast<Pool*>(AbstractDecl);

    std::string Name(NameTok.Start, NameTok.Length);

    // Evaluate the value string with the current top-level bindings.
    std::string Value(evalString(ValueTok, getCurrentBindings()));

    if (Name == "depth") {
      const char* Start = Value.c_str();
      char* End;
      long IntValue = ::strtol(Start, &End, 10);
      if (*End != '\0' || IntValue <= 0) {
        error("invalid depth", ValueTok);
      } else {
        Decl->setDepth(static_cast<uint32_t>(IntValue));
      }
    } else {
      error("unexpected variable", NameTok);
    }
  }

  virtual void actOnEndPoolDecl(PoolResult AbstractDecl,
                                const Token& StartTok) override {
    Pool* Decl = static_cast<Pool*>(AbstractDecl);

    // It is an error to not specify the pool depth.
    if (Decl->getDepth() == 0) {
      error("missing 'depth' variable assignment", StartTok);
    }
  }

  virtual RuleResult actOnBeginRuleDecl(const Token& NameTok) override {
    std::string Name(NameTok.Start, NameTok.Length);

    // Find the hash slot.
    auto& Result = TheManifest->getRules()[Name];

    // Diagnose if the rule already exists (we still create a new one).
    if (Result.get()) {
      // The rule already exists.
      error("duplicate rule", NameTok);
    }

    // Insert the new rule.
    Rule* Decl = new Rule(Name);
    Result.reset(Decl);
    return static_cast<RuleResult>(Decl);
  }

  virtual void actOnRuleBindingDecl(RuleResult AbstractDecl,
                                    const Token& NameTok,
                                    const Token& ValueTok) override {
    Rule* Decl = static_cast<Rule*>(AbstractDecl);

    std::string Name(NameTok.Start, NameTok.Length);
    // FIXME: It probably should be an error to assign to the same parameter
    // multiple times, but Ninja doesn't diagnose this.
    if (Rule::isValidParameterName(Name)) {
      Decl->getParameters()[Name] = std::string(ValueTok.Start,
                                                ValueTok.Length);
    } else {
      error("unexpected variable", NameTok);
    }
  }

  virtual void actOnEndRuleDecl(RuleResult AbstractDecl,
                                const Token& StartTok) override {
    Rule* Decl = static_cast<Rule*>(AbstractDecl);

    if (!Decl->getParameters().count("command")) {
      error("missing 'command' variable assignment", StartTok);
    }
  }

  /// @}
};

}

#pragma mark - ManifestLoader

ManifestLoader::ManifestLoader(std::string Filename,
                               ManifestLoaderActions &Actions)
  : Impl(static_cast<void*>(new ManifestLoaderImpl(Filename, Actions)))
{
}

ManifestLoader::~ManifestLoader() {
  delete static_cast<ManifestLoaderImpl*>(Impl);
}

std::unique_ptr<Manifest> ManifestLoader::load() {
  // Initialize the actions.
  static_cast<ManifestLoaderImpl*>(Impl)->getActions().initialize(this);

  return static_cast<ManifestLoaderImpl*>(Impl)->load();
}

const Parser* ManifestLoader::getCurrentParser() const {
  return static_cast<const ManifestLoaderImpl*>(Impl)->getCurrentParser();
}
