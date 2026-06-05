// Protect this translation unit from Windows-style min/max function-like macros.
// Some business environments define min/max before including project headers;
// those macros break std::min/std::max and std::numeric_limits<T>::max().
#if defined(min)
#pragma push_macro("min")
#undef min
#define REFLECTGEN_RESTORE_MIN_MACRO_ 1
#endif
#if defined(max)
#pragma push_macro("max")
#undef max
#define REFLECTGEN_RESTORE_MAX_MACRO_ 1
#endif

#include <clang-c/Index.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <streambuf>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

namespace
{
    struct BaseInfo
    {
        // Actual base specifier spelling as seen by libclang.
        // For template bases this may contain the concrete arguments, e.g. "Base<int>".
        std::string typeName;
        // Canonical type spelling when libclang can resolve aliases.  This is used
        // only for dependency discovery; generated static_cast expressions still
        // use typeName so template instances keep their concrete arguments.
        std::string canonicalTypeName;
        // Fully-qualified declaration name of the base class template/class, when libclang can resolve it.
        // Example: typeName may be "Base<int>", while qualifiedTypeName is "ns::Base".
        // Note that qualifiedTypeName intentionally does not contain template arguments.
        std::string qualifiedTypeName;
        CX_CXXAccessSpecifier access = CX_CXXInvalidAccessSpecifier;
    };

    struct FieldInfo
    {
        std::string name;
        // Original field type spelling, as emitted by libclang.
        std::string typeName;
        // Canonical field type spelling.  This lets root-class closure see through
        // typedef/using aliases such as PayloadAlias -> Payload.
        std::string canonicalTypeName;
        // Fully-qualified declaration name for the field's underlying record type
        // when libclang can resolve it.  This is especially useful for pointers,
        // references, typedef aliases, and wrapped template arguments.
        std::string declQualifiedName;
        CX_CXXAccessSpecifier access = CX_CXXInvalidAccessSpecifier;
        bool isBitField = false;
        int bitWidth = -1;
    };

    struct TemplateParamInfo
    {
        std::string declText;
        std::string argText;
    };

    enum class RecordTemplateKind
    {
        None,
        Primary,
        ExplicitSpecialization
    };

    struct RecordInfo
    {
        std::string qualifiedName;
        std::string simpleName;
        // Canonical source header path where the reflected type is declared.
        // In batch aggregate mode this is used to split one parsed TranslationUnit
        // back into per-source generated reflection headers.
        std::string sourcePath;
        // Direct semantic parent record name for nested classes/structs.
        // Used to emit public aliases from wave::ReflectAccess<Parent> when the
        // nested type is private/protected and cannot be named directly outside
        // the parent.
        std::string semanticParentQualifiedName;
        bool accessPathPublic = true;
        bool useReflectAccessAlias = false;
        std::string reflectAccessAliasName;
        bool isStruct = false;
        // True only for C-style anonymous typedef records such as:
        //   typedef struct { ... } Foo;
        // Such aliases do not have a real tag name, so generated type references
        // must use ::Foo rather than an elaborated type specifier like struct ::Foo.
        bool isAnonymousTypedefAlias = false;
        bool allowPrivateReflect = false;
        RecordTemplateKind templateKind = RecordTemplateKind::None;
        std::vector<TemplateParamInfo> templateParams;
        std::string explicitReflectedType;
        std::vector<BaseInfo> bases;
        std::vector<FieldInfo> fields;
    };

    struct Options
    {
        // Single-file mode:
        //   inputHeader  -> source header
        //   outputHeader -> generated header
        // Batch mode:
        //   batchDirs    -> root directories to scan
        //   outputHeader -> output directory
        //   aggregateHeader -> generated umbrella header
        std::string inputHeader;
        std::string outputHeader;
        std::string headerInclude;
        bool mainFileOnly = true;
        std::vector<std::string> clangArgs;

        bool batchMode = false;
        bool batchRecursive = true;
        bool batchRecursiveSpecified = false;
        bool batchFromDirListFile = false;
        std::vector<std::string> batchDirs;
        std::string batchDirListFile;
        std::string aggregateHeader;

        // Optional aggregate/business umbrella header used as the reflection target whitelist.
        // When set, batch mode reads direct quoted #include "*.h/*.hpp/*.hh/*.hxx"
        // lines from this header and uses those files as the whitelist. Angle-bracket
        // system includes are intentionally ignored. The header itself is used as
        // the temporary aggregate parse entry so its original include order and macros
        // are preserved.
        std::string includeWhitelistHeader;

        // Optional extra target list. One entry per line. Each line can be either
        // a plain header path, a directory path, or a quoted include directive
        // such as #include "L2L3C/vsiL2Top.h". When a directory is listed, all
        // .h/.hpp/.hh/.hxx files directly under it are added. Angle-bracket
        // include directives are ignored so system headers never become reflection
        // targets. Directory entries are scanned at the current level only.
        // The list supplements the aqcm.hpp whitelist, and can also be used
        // as the only target source when aqcm.hpp is not available.
        std::string extraTargetListFile;

        // Optional root-class closure mode.  When at least one root class is
        // specified, ReflectGen still parses the aqcm/aggregate environment, but
        // emitted records are restricted to the root classes and the business
        // classes reachable from their member field types.  If no root class is
        // specified, the legacy file-whitelist emission logic is used.
        std::vector<std::string> rootClassNames;
        std::string rootClassListFile;

        // Hardcoded-project defaults used to keep the command line short for the
        // common cmodel workflow.  When --vcxproj is provided without explicit
        // batch roots / -o / --log-file / --whitelist-from-header, ReflectGen
        // automatically uses:
        //   <vcxproj_dir>/inc/aqcm.hpp
        //   <vcxproj_dir>/generated_reflect
        //   <output_dir>/reflectgen.log
        bool autoIncludeWhitelistHeader = false;
        bool autoExtraTargetListFile = false;
        bool autoOutputHeader = false;
        bool autoLogFile = false;

        // Optional Visual Studio project import. When set, ReflectGen reads the
        // selected .vcxproj and derives libclang parse arguments from the
        // project's include directories, predefined macros, forced includes,
        // and C++ language standard. The headers to reflect are still chosen
        // by batchDirs / batchDirListFile, not by the project file.
        std::string vcxprojPath;
        std::string configuration = "Release";
        std::string platform = "x64";

        // Optional self-managed log file. This avoids relying on cmd.exe redirection
        // such as "> log.txt 2>&1" when launching from Visual Studio.
        std::string logFile;

        // Optional clang++ executable used only for diagnostics when libclang fails
        // before producing a usable TranslationUnit. The normal reflection path still
        // uses libclang.
        std::string clangxxPath;

        // Heavy AST / argument / include-tree diagnostics. Disabled by default so
        // production logs stay compact. Enable only when investigating why a
        // specific declaration was skipped.
        bool debugAst = false;

        // By default, a TranslationUnit that contains error/fatal diagnostics is
        // treated as unusable even if libclang returns a partial AST under
        // CXTranslationUnit_KeepGoing. Pass --allow-errors only for emergency
        // debugging where partial generated reflection is explicitly acceptable.
        bool allowParseErrors = false;

        // Optional heavy fallback for macro-heavy class declarations: run clang++ -E
        // to produce a fully macro-expanded temporary TU, parse it, and then mark
        // records that semantically/textually contain a real
        // friend ::wave::ReflectAccess / reflected_visitor declaration.
        // Disabled by default. Enable only with --expanded-friend-scan when the
        // cheap AST/token friend detection is insufficient for a specific header.
        bool expandedFriendScan = false;

        std::vector<std::string> boolStorageTypedefs;
    };

    struct CollectContext
    {
        bool mainFileOnly = true;
        // Batch aggregate mode parses one temporary umbrella header with recursive
        // traversal enabled, but only records whose spelling file is in this
        // whitelist are allowed to be reflected. This prevents STL/SystemC/third-party
        // include trees from becoming reflection targets.
        bool useFileWhitelist = false;
        std::set<std::string> sourceFileWhitelist;
        std::set<std::string> visited;
        std::vector<RecordInfo> records;
        CXTranslationUnit tu = NULL;

        // Record collection diagnostics. These counters are intentionally kept
        // here rather than in globals so batch mode reports one summary per
        // parsed header. They make it obvious whether libclang did not see any
        // class/struct cursor at all, or whether ReflectGen saw them and then
        // skipped them because of main-file filtering, empty spelling,
        // sc_core/reflect infrastructure filtering, or duplicate visit keys.
        std::size_t classTemplateCandidates = 0;
        std::size_t classTemplateCollected = 0;
        std::size_t classTemplateSkipped = 0;
        std::size_t recordCandidates = 0;
        std::size_t recordCollected = 0;
        std::size_t recordSkipped = 0;
        std::size_t recordDuplicates = 0;
        std::size_t basesKept = 0;
        std::size_t basesSkippedSystemC = 0;
        std::size_t fieldsKept = 0;
        std::size_t fieldsSkippedEmptyName = 0;
        std::size_t fieldsSkippedSystemC = 0;
    };

    bool gDebugAst = false;

    static const char* kReflectFriendMarkerTypeAliasName = "wave_reflect_friend_marker_do_not_use";

    struct SourceOffsetLocation
    {
        std::string filePath;
        unsigned offset = 0;
        bool valid = false;
    };

    struct ReflectFriendMacroExpansionRange
    {
        std::string filePath;
        unsigned beginOffset = 0;
        unsigned endOffset = 0;
        std::string macroName;
    };

    // Hot-path caches. The friend check is called for records, nested records,
    // and access-path checks. Without these caches, the macro-owner fallback can
    // rescan all preprocessing cursors for every record and repeatedly normalize
    // the same file paths.
    std::map<std::string, std::string> gCanonicalSourcePathCache;
    std::map<std::string, std::string> gSourceFileTextCache;
    std::map<std::string, bool> gReflectFriendCursorCache;
    std::map<const void*, std::vector<ReflectFriendMacroExpansionRange> > gReflectFriendMacroRangeCache;

    struct EmittedMember
    {
        std::string displayName;
        std::string typeName;
        bool isBitField = false;
        int bitWidth = -1;
        bool exprIsPointerAlready = false;
        bool asBoolStorage = false;
        std::string constExpr;
        std::string mutExpr;
    };
    // Centralized forward declarations for file-local helpers.
    // Keep these declarations before any helper definition so later edits do not
    // become sensitive to function definition order.
    struct DiagnosticsSummary;
    enum class IncludeDirectiveKind;
    struct ActiveIncludeWhitelistState;
    struct BatchHeaderJob;

    std::string ToStdString(CXString s);

    std::string EscapeString(const std::string& s);

    std::string MakeHeaderGuard(const std::string& path);

    std::string Trim(const std::string& s);

    bool ConsumePrefix(std::string& s, const std::string& prefix);

    std::string NormalizeBaseDisplayName(std::string name);

    std::string CanonicalRecordLookupKey(std::string typeName);

    std::string GetBaseName(const std::string& path);

    bool StartsWithDash(const std::string& s);

    CX_CXXAccessSpecifier NormalizeAccess(CX_CXXAccessSpecifier access, bool isStruct);

    bool IsPublicAccess(CX_CXXAccessSpecifier access);

    std::string GetCursorSourceText(CXTranslationUnit tu, CXCursor cursor);

    std::string ShortenForDebugLog(std::string text, std::size_t maxLen);

    bool IsReflectFriendIdentifier(const std::string& spelling, bool allowExpandedFriendNames);

    bool TokenRangeHasReflectFriend(CXTranslationUnit tu,
                                    CXSourceRange range,
                                    bool allowExpandedFriendNames,
                                    std::string* matchedToken);

    bool CursorTokensHaveReflectFriend(CXTranslationUnit tu,
                                       CXCursor cursor,
                                       bool allowExpandedFriendNames,
                                       std::string* matchedToken);

    bool CursorHasReflectFriendMarkerDecl(CXCursor cursor);

    bool SourceLocationExpansionOffset(CXSourceLocation loc,
                                       std::string* filePath,
                                       unsigned* offset);

    bool SourceRangeContainsExpansionLocation(CXSourceRange range,
                                              CXSourceLocation loc);

    bool FillSourceOffsetLocation(CXSourceLocation loc, SourceOffsetLocation* out);

    bool SourceOffsetInRange(const SourceOffsetLocation& loc,
                             const ReflectFriendMacroExpansionRange& range);

    std::string CursorReflectFriendCacheKey(CXTranslationUnit tu, CXCursor cursor);

    const std::vector<ReflectFriendMacroExpansionRange>& GetReflectFriendMacroExpansionRanges(CXTranslationUnit tu);

    bool MacroDefinitionAllowsReflectFriend(CXTranslationUnit tu, CXCursor macroDef);

    bool CursorOwningMacroExpansionAllowsReflectFriend(CXTranslationUnit tu, CXCursor recordCursor);

    bool FriendDeclAllowsReflectAccess(CXTranslationUnit tu, CXCursor cursor);

    bool TextHasReflectFriendMacroOutsideCommentsAndLiterals(const std::string& text);

    std::string GetCursorRawSourceTextFromFile(CXCursor cursor);

    const std::string& GetSourceFileTextCached(const std::string& path);

    std::size_t FindNextCharOutsideCommentsAndLiterals(const std::string& text,
                                                       std::size_t start,
                                                       char target);

    std::size_t FindMatchingBraceOutsideCommentsAndLiterals(const std::string& text,
                                                            std::size_t openBrace);

    bool CursorBodySourceHasReflectFriend(CXCursor cursor, std::string* matchedToken);

    bool CursorExpansionLocationIsFromMainFile(CXTranslationUnit tu, CXCursor cursor);

    bool CursorHasReflectAccessFriend(CXTranslationUnit tu, CXCursor cursor);

    bool RecordAllowsAccess(const RecordInfo& rec, CX_CXXAccessSpecifier access);

    void PrintUsage();

    bool ParseCommandLine(int argc, char** argv, Options& opt);

    bool IsRecordKind(CXCursorKind kind);

    std::string GetQualifiedName(CXCursor cursor);

    std::string CanonicalSourcePathKey(std::string path);

    std::string GetCursorSourceFilePath(CXCursor cursor);

    bool CursorPassesSourceWhitelist(CXCursor cursor, const CollectContext& ctx);

    bool IsRecordLikeCursorKind(CXCursorKind kind);

    bool IsNestedInsideRecordLike(CXCursor cursor, CXCursor* parentOut);

    bool IsUnionCursorKind(CXCursorKind kind);

    bool IsInsideUnionByParentChain(CXCursor cursor, bool semantic);

    bool IsInsideUnion(CXCursor cursor);

    bool IsUnionTypeCursor(CXCursor cursor);

    bool IsUnionFieldCursor(CXCursor fieldCursor);

    bool CursorAccessPathIsPublicOrTopLevel(CXCursor cursor);

    bool CursorAccessIsPublicOrTopLevel(CXCursor cursor);

    bool CursorAccessPathIsReflectAccessible(CXTranslationUnit tu, CXCursor cursor);

    bool CursorAccessIsReflectAccessible(CXTranslationUnit tu, CXCursor cursor);

    CXType StripTypeForAccessPathCheck(CXType type);

    bool FieldTypeAccessPathIsReflectAccessible(CXTranslationUnit tu, CXCursor fieldCursor);

    bool IsDependentOrTemplateParamFieldType(CXCursor fieldCursor);

    bool RecordHasExplicitTagNameByTokens(CXTranslationUnit tu, CXCursor cursor);

    std::string GetCursorKindName(CXCursorKind kind);

    std::string GetCursorLocString(CXCursor cursor);

    std::string GetCursorSpellingLocString(CXCursor cursor);

    std::string GetCursorDisplayName(CXCursor cursor);

    std::string GetCursorTypeNameForDebug(CXCursor cursor);

    std::string GetCursorUsrForDebug(CXCursor cursor);

    bool LooksLikeLibclangAnonymousName(const std::string& s);

    void PrintCursorDebugLine(const char* tag, CXCursor cursor, const std::string& extra);

    std::string GetRecordSkipReason(CXCursor cursor, const CollectContext& ctx);

    std::string GetTemplateRecordSkipReason(CXCursor cursor, const CollectContext& ctx);

    bool ShouldPrintSkipReason(const std::string& reason);

    void PrintRecordCollectionSummary(const Options& opt, const CollectContext& ctx);

    bool IsUnderNamespaceNamed(CXCursor cursor, const std::string& namespaceName);

    bool IsVsipPortRecordCursor(CXCursor cursor);

    bool IsNonBusinessRecordCursor(CXCursor cursor);

    std::string NormalizeTypeLookupKey(const std::string& typeName);

    bool IsScCoreTypeName(const std::string& typeName);

    CXType StripCvPointerReferenceArrayType(CXType type);

    std::string GetTypeDeclarationQualifiedName(CXType type);

    bool IsWhitelistedSystemCMemberType(const std::string& typeName);

    bool ShouldSkipFieldType(const std::string& typeName);

    bool IsSystemCBaseInfo(const BaseInfo& b);

    bool IsExplicitTemplateSpecialization(CXCursor cursor);

    bool ShouldSkipRecord(CXCursor cursor, const CollectContext& ctx);

    void CollectRecordBody(CXTranslationUnit tu, CXCursor recordCursor, RecordInfo& rec, CollectContext* ctx);

    bool CollectTemplateRecord(CXTranslationUnit tu, CXCursor cursor, CollectContext& ctx, RecordInfo& rec);

    bool CollectAnonymousTypedefRecord(CXTranslationUnit tu, CXCursor typedefCursor, CollectContext& ctx, RecordInfo& rec);

    CXChildVisitResult AstVisitor(CXCursor cursor, CXCursor, CXClientData clientData);

    DiagnosticsSummary PrintDiagnostics(CXTranslationUnit tu);

    bool DiagnosticsAreAcceptableOrReport(const Options& opt, const DiagnosticsSummary& diag);

    const char* CxErrorName(CXErrorCode err);

    void PrintClangArgsForLog(const std::vector<std::string>& args);

    std::vector<std::string> BuildDiagnosticProbeArgsAndExtractForcedIncludes(
        const Options& opt,
        std::vector<std::string>& forcedIncludes);

    void RunLibclangDiagnosticProbe(CXIndex index, const Options& opt);

    std::string ShellQuote(const std::string& raw);

    std::string GetEnvStringForClangxx(const char* name);

    std::string JoinSimplePathForClangxx(std::string a, const std::string& b);

    std::string ResolveClangxxPath(const Options& opt);

    std::vector<std::string> BuildClangxxDiagnosticArgs(const Options& opt);

    bool FileExistsForDiagnostics(const std::string& path);

    bool LooksLikeExplicitPath(const std::string& path);

    std::string AbsoluteDiagnosticPathOrEmpty(std::string path);

    std::string CaptureProcessOutput(const std::string& cmd, int& rc);

    void PrintPathCheck(const std::string& label, const std::string& path);

    std::string LastSystemErrorForLog();

    bool DirectoryExistsForDiagnostics(const std::string& path);

    void PrintPathFailureContext(const std::string& label, const std::string& path);

    void PrintDirectoryCreateFailure(const std::string& label, const std::string& dir);

    void PrintFileOpenFailure(const std::string& label, const std::string& path);

    bool FlushAndCheckFile(std::ofstream& os, const std::string& path, const std::string& label);

    void RunClangxxDiagnosticProbe(const Options& opt);

    std::string EnsureGlobalQualifiedType(std::string typeName);

    std::string StripLeadingGlobalQualifier(std::string typeName);

    std::string BuildTemplateArgListForRecord(const RecordInfo& rec);

    std::string BuildReflectedTypeNameUnqualifiedRoot(const RecordInfo& rec);

    std::string BuildReflectedTypeNameGlobal(const RecordInfo& rec);

    std::string BuildElaboratedReflectedTypeNameGlobal(const RecordInfo& rec);

    const RecordInfo* FindRecord(const std::map<std::string, const RecordInfo*>& byName, const std::string& typeName);

    const RecordInfo* FindRecordForBase(const std::map<std::string, const RecordInfo*>& byName, const BaseInfo& base);

    std::string StripClassKeyQualifiersFromTypeSpelling(std::string name);

    std::string BuildActualBaseTypeNameGlobal(const BaseInfo& base);

    std::string BuildBaseTypeForEmission(const BaseInfo& base, const RecordInfo* baseRec);

    void CollectMembersRecursive(
        const Options& opt,
        const RecordInfo& rec,
        const std::map<std::string, const RecordInfo*>& byName,
        const std::string& constObjExpr,
        const std::string& mutObjExpr,
        std::set<std::string>& stack,
        std::vector<EmittedMember>& out);

    std::vector<EmittedMember> BuildFlattenedMembers(const Options& opt, const RecordInfo& rec, const std::map<std::string, const RecordInfo*>& byName);

    std::string BuildTemplatePrefix(const RecordInfo& rec);

    std::string BuildTemplateArgList(const RecordInfo& rec);

    std::string BuildClassSpecializationPrefix(const RecordInfo& rec);

    std::string BuildVisitObjectTemplatePrefix(const RecordInfo& rec);

    std::string BuildBitGetterName(const std::string& fieldDisplayName);

    std::string BuildReflectedType(const RecordInfo& rec);

    std::string StripExtension(const std::string& path);

    std::string SanitizeIdentifier(const std::string& text);

    std::string GetOutputStem(const Options& opt);

    std::string NormalizePathSlashes(std::string path);

    bool IsAbsolutePath(const std::string& path);

    std::string JoinPath(const std::string& a, const std::string& b);

    std::string DirectoryName(const std::string& path);

    std::string FileNameOnly(const std::string& path);

    bool HasHeaderExtension(const std::string& path);

    bool HasHOrHppExtension(const std::string& path);

    std::string ToLowerAscii(std::string s);

    bool EndsWithString(const std::string& s, const std::string& suffix);

    bool ContainsPathComponent(const std::string& normalizedPath, const std::string& componentLower);

    bool IsGeneratedReflectHeaderName(const std::string& lowerFileName);

    bool IsReflectionSystemHeaderCandidate(const std::string& path);

    std::string ReplaceHeaderExtensionWithReflectAuto(const std::string& rel);

    bool StartsWithPathPrefix(const std::string& path, const std::string& prefix);

    std::string MakeRelativeToRoot(const std::string& path, const std::string& root);

    std::string StripInlineCommentAndTrim(const std::string& line);

    std::string GetEnvVarForPathExpansion(const std::string& name);

    std::string ExpandEnvironmentVariablesInPath(std::string value);

    bool ReadBatchDirListFile(const std::string& path, std::vector<std::string>& dirs);

    std::string CurrentWorkingDirectory();

    std::vector<std::string> SplitPathParts(const std::string& path);

    std::string PathRootToken(const std::string& path);

    std::string StripPathRootForParts(const std::string& path);

    std::string CollapseDotDotPath(const std::string& path);

    std::string MakeAbsoluteLexicalPath(const std::string& path);

    std::string MakeRelativePathLexical(const std::string& fromDir, const std::string& toPath);

    std::string MakeGeneratedHeaderIncludeText(const std::string& generatedHeader, const std::string& inputHeader);

    std::string ReadWholeFileText(const std::string& path);

    std::string DecodeXmlEntities(std::string s);

    std::vector<std::string> SplitSemicolonList(const std::string& text);

    std::string StripOuterQuotes(std::string s);

    std::vector<std::string> TokenizeCommandLineLike(const std::string& text);

    std::vector<std::string> ExtractXmlTagValues(const std::string& block, const std::string& tag);

    std::string ExtractXmlAttribute(const std::string& tagText, const std::string& attr);

    bool ConditionMatchesConfigPlatform(const std::string& startTag, const std::string& config, const std::string& platform);

    std::vector<std::string> ExtractMatchingBlocks(const std::string& xml,
        const std::string& tag,
        const std::string& config,
        const std::string& platform);

    std::string EnsureTrailingSlash(std::string path);

    std::string GetEnvVarString(const std::string& name);

    void ReplaceAll(std::string& s, const std::string& from, const std::string& to);

    std::string ExpandMsbuildMacros(std::string value,
        const std::string& projectDir,
        const std::string& config,
        const std::string& platform);

    bool ContainsUnexpandedMacro(const std::string& s);

    bool ContainsUnexpandedPercentEnv(const std::string& s);

    bool ContainsAnyUnexpandedVariable(const std::string& s);

    bool IsClangSeparateValueOption(const std::string& arg);

    bool IsPathLikeClangArg(const std::string& arg);

    std::string ExpandUserClangArg(std::string arg);

    void AddRawClangArg(std::vector<std::string>& args, const std::string& arg);

    void AddExpandedUserClangArgs(std::vector<std::string>& merged, const std::vector<std::string>& userArgs);

    bool HasArgPrefix(const std::vector<std::string>& args, const std::string& prefix);

    bool HasExactArg(const std::vector<std::string>& args, const std::string& exact);

    void AddUniqueArg(std::vector<std::string>& args, const std::string& arg);

    std::string MapLanguageStandardToClang(const std::string& value);

    void AddIncludeDirArg(std::vector<std::string>& args, std::string dir, const std::string& projectDir,
        const std::string& config, const std::string& platform);

    void AddDefineArg(std::vector<std::string>& args, std::string def, const std::string& projectDir,
        const std::string& config, const std::string& platform);

    void AddForcedIncludeArg(std::vector<std::string>& args, std::string file, const std::string& projectDir,
        const std::string& config, const std::string& platform);

    void ParseAdditionalOptionsIntoArgs(std::vector<std::string>& args,
        const std::string& text,
        const std::string& projectDir,
        const std::string& config,
        const std::string& platform);

    bool ApplyVcxprojSettings(Options& opt);

    std::string MakeFlatReflectAutoFileName(const std::string& input, const std::string& root);

    bool MakeDirectoryOne(const std::string& dir);

    bool MakeDirectoryRecursive(const std::string& dir);

    bool IsSameCanonicalPath(const std::string& a, const std::string& b);

    bool IsCanonicalAncestorOrSelf(const std::string& ancestorRaw, const std::string& childRaw);

    bool IsRootLikePathForClear(const std::string& path);

    bool IsSafeBatchOutputDirectoryToClear(const std::string& outputDir,
        const std::vector<std::string>& roots,
        std::string& reason);

    bool ShouldPreservePathDuringClear(const std::set<std::string>& preservePaths, const std::string& path);

    bool ClearDirectoryContentsRecursive(const std::string& dir,
        const std::set<std::string>& preservePaths,
        const std::string& label);

    bool ClearBatchOutputDirectoryBeforeEmit(const std::string& outputDir,
        const std::vector<std::string>& roots,
        const Options& opt);

    void CollectHeaderFilesInDirectory(const std::string& root, bool recursive, bool hOrHppOnly, std::vector<std::string>& out);

    std::string MakeAggregateIncludePath(const std::string& aggregateHeader, const std::string& generatedHeader);

    std::string BuildWaveRegistrationFunctionName(const Options& opt);

    std::string BuildConvenienceRegistrationFunctionName(const Options& opt);

    int QualifiedNameDepth(const std::string& qname);

    std::string MakeReflectAccessAliasName(const RecordInfo& rec);

    bool EmitGeneratedFile(const std::vector<RecordInfo>& records, const Options& opt, const std::map<std::string, const RecordInfo*>* globalByName);

    bool GenerateOneHeader(const Options& baseOpt,
        const std::string& inputHeader,
        const std::string& outputHeader,
        const std::string& headerInclude,
        std::size_t& recordCount);

    bool EmitAggregateHeader(const std::string& aggregateHeader,
        const std::vector<std::string>& generatedHeaders);

    std::map<std::string, const RecordInfo*> BuildRecordLookupMap(const std::vector<RecordInfo>& records);

    bool IsBuiltinOrNonRecordLookupKey(const std::string& key);

    bool IsStdClosureStopType(const std::string& key);

    std::string StripTypeForClosure(std::string s);

    std::string TopLevelTypeKeyForClosure(const std::string& typeName);

    std::vector<std::string> SplitTopLevelTemplateArgs(const std::string& typeName);

    void AddUniqueClosureCandidate(std::vector<std::string>& out, const std::string& candidate);

    void AddClosureTypeCandidatesFromTypeName(const std::string& typeName, std::vector<std::string>& out);

    void AddClosureTypeCandidatesFromField(const FieldInfo& field, std::vector<std::string>& out);

    void AddClosureTypeCandidatesFromBase(const BaseInfo& base, std::vector<std::string>& out);

    std::vector<std::string> ReadRootClassNamesFromFile(const Options& opt);

    std::vector<std::string> GetRequestedRootClassNames(const Options& opt);

    std::vector<RecordInfo> ComputeRootClassClosureRecords(const std::vector<RecordInfo>& allRecords,
        const std::vector<std::string>& rootNames);

    bool WriteBatchAggregateInputHeader(const std::string& path, const std::vector<std::string>& headers);

    bool CollectRecordsFromTranslationUnit(const Options& opt,
        CXTranslationUnit tu,
        bool mainFileOnly,
        bool useWhitelist,
        const std::set<std::string>& whitelist,
        std::vector<RecordInfo>& records);

    bool IsClangXLanguageSelector(const std::string& arg);

    bool IsClangOutputOrCompileOnlyArg(const std::string& arg);

    std::vector<std::string> BuildClangxxPreprocessArgs(const Options& opt, const std::string& expandedPath);

    std::vector<std::string> BuildExpandedParseArgs(const Options& opt);

    std::string ReflectGenTempBaseForExpandedFriendScan(const Options& opt);

    bool WriteCommandResponseFile(const std::string& rspPath,
                                  const std::vector<std::string>& args,
                                  const std::string& input);

    bool RunExpandedFriendPreprocess(const Options& opt, std::string& expandedPath);

    void CollectExpandedFriendRecordsFromCursor(CXTranslationUnit tu, CXCursor cursor, std::set<std::string>& out);

    bool CollectExpandedReflectFriendRecords(const Options& opt, std::set<std::string>& expandedFriendRecords);

    void ApplyExpandedReflectFriendRecords(std::vector<RecordInfo>& records, const std::set<std::string>& expandedFriendRecords);

    bool ParseAndCollectRecords(const Options& opt,
        bool mainFileOnly,
        bool useWhitelist,
        const std::set<std::string>& whitelist,
        std::vector<RecordInfo>& records);

    bool IsExistingRegularFile(const std::string& path);

    bool HasWildcardChars(const std::string& path);

    bool ExtractNextXmlStartTag(const std::string& xml, std::size_t& pos, std::string& tagText);

    std::string ResolveProjectHeaderItemPath(std::string item,
        const std::string& projectDir,
        const std::string& config,
        const std::string& platform);

    bool IsExternalDependenciesFilterName(std::string filter);

    std::string MakeVcxprojFiltersPath(const std::string& vcxprojPath);

    bool ExtractNextXmlElementBlock(const std::string& xml,
        const std::string& tag,
        std::size_t& pos,
        std::string& startTag,
        std::string& body);

    std::string ExtractFirstXmlTagValueTrimmed(const std::string& block, const std::string& tag);

    void CollectHeaderFilesFromVcxproj(const Options& opt,
        std::vector<std::string>& outHeaders,
        std::string& projectDirOut);

    std::vector<std::string> ExtractIncludeDirsFromClangArgs(const Options& opt, const std::string& baseDir);

    std::string StripCxxCommentsForIncludeParse(const std::string& line, bool& inBlockComment);

    IncludeDirectiveKind ExtractIncludeDirectiveTarget(const std::string& rawLine, std::string& target);

    std::string ResolveQuotedIncludeFromHeader(const std::string& includeText,
        const std::string& headerDir,
        const std::vector<std::string>& includeDirs,
        const Options& opt);

    std::string ResolveExistingFileOrDirectoryFromListEntry(const std::string& entryText,
        const std::string& listDir,
        const std::vector<std::string>& includeDirs,
        const Options& opt);

    std::vector<std::string> SplitLinesPreserveEmpty(const std::string& text);

    CXChildVisitResult ActiveIncludeWhitelistVisitor(CXCursor cursor, CXCursor, CXClientData clientData);

    void CollectHeaderFilesFromIncludeWhitelistHeader(const Options& opt,
        std::vector<std::string>& outHeaders,
        std::string& rootOut,
        std::string& parseEntryHeaderOut);

    bool LooksLikePlainTargetListEntry(const std::string& line);

    void CollectHeaderFilesFromExtraTargetListFile(const Options& opt,
        std::vector<std::string>& outHeaders,
        std::string& rootOut);

    bool RunBatchMode(const Options& opt);

    std::string FindEarlyOptionValue(int argc, char** argv, const std::string& optionName);

    bool ApplyHardcodedProjectDefaults(Options& opt);

    // End centralized forward declarations.


    std::string ToStdString(CXString s)
    {
        const char* p = clang_getCString(s);
        std::string out = p ? p : "";
        clang_disposeString(s);
        return out;
    }

    std::string EscapeString(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 16);
        for (char c : s)
        {
            switch (c)
            {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
            }
        }
        return out;
    }

    std::string MakeHeaderGuard(const std::string& path)
    {
        std::string g = "REFLECT_GENERATED_";
        for (char c : path)
        {
            g += std::isalnum(static_cast<unsigned char>(c))
                ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                : '_';
        }
        g += "_H";
        return g;
    }

    std::string Trim(const std::string& s)
    {
        std::size_t b = 0;
        while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        std::size_t e = s.size();
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        return s.substr(b, e - b);
    }

    bool ConsumePrefix(std::string& s, const std::string& prefix)
    {
        if (s.compare(0, prefix.size(), prefix) == 0)
        {
            s.erase(0, prefix.size());
            return true;
        }
        return false;
    }

    std::string NormalizeBaseDisplayName(std::string name)
    {
        name = Trim(name);

        bool changed = true;
        while (changed)
        {
            changed = false;
            changed = ConsumePrefix(name, "::") || changed;
            changed = ConsumePrefix(name, "class ") || changed;
            changed = ConsumePrefix(name, "struct ") || changed;
            changed = ConsumePrefix(name, "const ") || changed;
            changed = ConsumePrefix(name, "volatile ") || changed;
            changed = ConsumePrefix(name, "enum ") || changed;
            name = Trim(name);
        }

        std::string out;
        out.reserve(name.size());
        for (char c : name)
        {
            if (!std::isspace(static_cast<unsigned char>(c))) out += c;
        }
        if (out.empty()) out = "Base";
        return out;
    }

    std::string CanonicalRecordLookupKey(std::string typeName)
    {
        typeName = Trim(typeName);
        bool changed = true;
        while (changed)
        {
            changed = false;
            changed = ConsumePrefix(typeName, "::") || changed;
            changed = ConsumePrefix(typeName, "class ") || changed;
            changed = ConsumePrefix(typeName, "struct ") || changed;
            changed = ConsumePrefix(typeName, "const ") || changed;
            changed = ConsumePrefix(typeName, "volatile ") || changed;
            changed = ConsumePrefix(typeName, "enum ") || changed;
            typeName = Trim(typeName);
        }

        // Build a lookup key for RecordInfo matching.  This key must preserve the
        // qualified type path while removing template arguments from every path
        // component.  The old implementation stopped at the first top-level '<',
        // which made bases such as Outer<int>::Base<float> collapse to just Outer
        // and therefore failed to find the RecordInfo for Outer::Base.
        //
        // Examples:
        //   Base<int>                    -> Base
        //   ns::Base<int>                -> ns::Base
        //   ns::Outer<int>::Base<float>  -> ns::Outer::Base
        //   const struct ::A<T>*         -> A
        std::string out;
        int angleDepth = 0;
        for (std::size_t i = 0; i < typeName.size(); ++i)
        {
            const char c = typeName[i];
            if ((c == '*' || c == '&') && angleDepth == 0) break;
            if (c == '<') { ++angleDepth; continue; }
            if (c == '>') { if (angleDepth > 0) --angleDepth; continue; }
            if (angleDepth > 0) continue;
            if (std::isspace(static_cast<unsigned char>(c))) continue;
            out += c;
        }

        // After template stripping, class-key tokens can appear after a scope token,
        // e.g. ns::class A on some MSVC/libclang spellings.  Remove them token-wise.
        const char* keys[] = { "class ", "struct ", "union ", "enum " };
        for (std::size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k)
        {
            const std::string key(keys[k]);
            std::size_t pos = 0;
            while ((pos = out.find(key, pos)) != std::string::npos)
            {
                const bool atTokenBoundary = (pos == 0) ||
                    !(std::isalnum(static_cast<unsigned char>(out[pos - 1])) || out[pos - 1] == '_');
                if (atTokenBoundary) out.erase(pos, key.size());
                else pos += key.size();
            }
        }
        return out;
    }

    std::string GetBaseName(const std::string& path)
    {
        const std::size_t p = path.find_last_of("/\\");
        return p == std::string::npos ? path : path.substr(p + 1);
    }

    bool StartsWithDash(const std::string& s)
    {
        return !s.empty() && s[0] == '-';
    }

    CX_CXXAccessSpecifier NormalizeAccess(CX_CXXAccessSpecifier access, bool isStruct)
    {
        if (access == CX_CXXInvalidAccessSpecifier)
        {
            return isStruct ? CX_CXXPublic : CX_CXXPrivate;
        }
        return access;
    }

    bool IsPublicAccess(CX_CXXAccessSpecifier access)
    {
        return access == CX_CXXPublic;
    }

    std::string GetCursorSourceText(CXTranslationUnit tu, CXCursor cursor)
    {
        std::string out;
        if (!tu) return out;
        CXSourceRange range = clang_getCursorExtent(cursor);
        CXToken* tokens = NULL;
        unsigned tokenCount = 0;
        clang_tokenize(tu, range, &tokens, &tokenCount);
        for (unsigned i = 0; i < tokenCount; ++i)
        {
            if (!out.empty()) out += ' ';
            out += ToStdString(clang_getTokenSpelling(tu, tokens[i]));
        }
        if (tokens != NULL) clang_disposeTokens(tu, tokens, tokenCount);
        return out;
    }

    std::string ShortenForDebugLog(std::string text, std::size_t maxLen = 360)
    {
        for (char& c : text)
        {
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        }
        if (text.size() > maxLen) text = text.substr(0, maxLen) + "...";
        return text;
    }

    bool IsReflectFriendIdentifier(const std::string& spelling, bool allowExpandedFriendNames)
    {
        // Macro spellings are accepted in record-body token fallback.
        if (spelling == "WAVE_REFLECT_FRIEND" ||
            spelling == "WAVE_REFLECT_ACCESS" ||
            spelling == "REFLECT_FRIEND")
        {
            return true;
        }

        // Expanded names are accepted only when scanning a real FriendDecl.
        // Do not accept them for whole-record fallback, otherwise unrelated
        // identifiers in method bodies could accidentally enable private reflect.
        if (allowExpandedFriendNames &&
            (spelling == "ReflectAccess" || spelling == "reflected_visitor"))
        {
            return true;
        }

        return false;
    }

    bool TokenRangeHasReflectFriend(CXTranslationUnit tu,
                                    CXSourceRange range,
                                    bool allowExpandedFriendNames,
                                    std::string* matchedToken)
    {
        if (matchedToken) matchedToken->clear();
        if (!tu) return false;

        CXToken* tokens = NULL;
        unsigned tokenCount = 0;
        clang_tokenize(tu, range, &tokens, &tokenCount);

        bool found = false;
        for (unsigned i = 0; i < tokenCount; ++i)
        {
            const CXTokenKind kind = clang_getTokenKind(tokens[i]);

            // Important: do not inspect comments, string literals, punctuation,
            // etc.  This prevents "// WAVE_REFLECT_FRIEND" or
            // "const char* s = \"WAVE_REFLECT_FRIEND\"" from enabling private reflection.
            if (kind != CXToken_Identifier) continue;

            const std::string spelling = ToStdString(clang_getTokenSpelling(tu, tokens[i]));
            if (IsReflectFriendIdentifier(spelling, allowExpandedFriendNames))
            {
                found = true;
                if (matchedToken) *matchedToken = spelling;
                break;
            }
        }

        if (tokens != NULL) clang_disposeTokens(tu, tokens, tokenCount);
        return found;
    }

    bool CursorTokensHaveReflectFriend(CXTranslationUnit tu,
                                       CXCursor cursor,
                                       bool allowExpandedFriendNames,
                                       std::string* matchedToken)
    {
        return TokenRangeHasReflectFriend(tu,
                                          clang_getCursorExtent(cursor),
                                          allowExpandedFriendNames,
                                          matchedToken);
    }

    bool FillSourceOffsetLocation(CXSourceLocation loc, SourceOffsetLocation* out)
    {
        if (!out) return false;
        out->filePath.clear();
        out->offset = 0;
        out->valid = false;

        CXFile file = NULL;
        unsigned line = 0;
        unsigned column = 0;
        unsigned off = 0;
        clang_getExpansionLocation(loc, &file, &line, &column, &off);
        if (!file) return false;

        const std::string path = CanonicalSourcePathKey(ToStdString(clang_getFileName(file)));
        if (path.empty()) return false;

        out->filePath = path;
        out->offset = off;
        out->valid = true;
        return true;
    }

    bool SourceLocationExpansionOffset(CXSourceLocation loc,
                                       std::string* filePath,
                                       unsigned* offset)
    {
        SourceOffsetLocation info;
        if (!FillSourceOffsetLocation(loc, &info)) return false;
        if (filePath) *filePath = info.filePath;
        if (offset) *offset = info.offset;
        return true;
    }

    bool SourceOffsetInRange(const SourceOffsetLocation& loc,
                             const ReflectFriendMacroExpansionRange& range)
    {
        if (!loc.valid) return false;
        if (loc.filePath.empty() || loc.filePath != range.filePath) return false;
        if (range.endOffset <= range.beginOffset) return loc.offset == range.beginOffset;
        return loc.offset >= range.beginOffset && loc.offset < range.endOffset;
    }

    bool SourceRangeContainsExpansionLocation(CXSourceRange range,
                                              CXSourceLocation loc)
    {
        ReflectFriendMacroExpansionRange r;
        SourceOffsetLocation beginInfo;
        SourceOffsetLocation endInfo;
        SourceOffsetLocation locInfo;
        if (!FillSourceOffsetLocation(clang_getRangeStart(range), &beginInfo)) return false;
        if (!FillSourceOffsetLocation(clang_getRangeEnd(range), &endInfo)) return false;
        if (!FillSourceOffsetLocation(loc, &locInfo)) return false;
        if (beginInfo.filePath.empty() || beginInfo.filePath != endInfo.filePath) return false;
        r.filePath = beginInfo.filePath;
        r.beginOffset = beginInfo.offset;
        r.endOffset = endInfo.offset;
        return SourceOffsetInRange(locInfo, r);
    }

    bool MacroDefinitionAllowsReflectFriend(CXTranslationUnit tu, CXCursor macroDef)
    {
        if (clang_Cursor_isNull(macroDef)) return false;
        if (clang_getCursorKind(macroDef) != CXCursor_MacroDefinition) return false;

        std::string matched;
        if (CursorTokensHaveReflectFriend(tu, macroDef, true, &matched)) return true;

        const std::string rawText = GetCursorRawSourceTextFromFile(macroDef);
        return TextHasReflectFriendMacroOutsideCommentsAndLiterals(rawText);
    }

    std::string CursorReflectFriendCacheKey(CXTranslationUnit tu, CXCursor cursor)
    {
        std::ostringstream oss;
        oss << static_cast<const void*>(tu) << '|'
            << static_cast<int>(clang_getCursorKind(cursor)) << '|'
            << ToStdString(clang_getCursorSpelling(cursor)) << '|';
        CXFile file = NULL;
        unsigned line = 0;
        unsigned column = 0;
        unsigned off = 0;
        clang_getExpansionLocation(clang_getCursorLocation(cursor), &file, &line, &column, &off);
        if (file)
        {
            oss << ToStdString(clang_getFileName(file)) << ':' << off;
        }
        else
        {
            oss << "nofile";
        }
        return oss.str();
    }

    const std::vector<ReflectFriendMacroExpansionRange>& GetReflectFriendMacroExpansionRanges(CXTranslationUnit tu)
    {
        const void* tuKey = static_cast<const void*>(tu);
        std::map<const void*, std::vector<ReflectFriendMacroExpansionRange> >::iterator it =
            gReflectFriendMacroRangeCache.find(tuKey);
        if (it != gReflectFriendMacroRangeCache.end()) return it->second;

        std::vector<ReflectFriendMacroExpansionRange> ranges;
        if (!tu)
        {
            gReflectFriendMacroRangeCache[tuKey] = ranges;
            return gReflectFriendMacroRangeCache[tuKey];
        }

        struct Payload
        {
            CXTranslationUnit tu;
            std::vector<ReflectFriendMacroExpansionRange>* ranges;
            std::map<std::string, bool> macroAllowCache;
        } payload;
        payload.tu = tu;
        payload.ranges = &ranges;

        CXCursor root = clang_getTranslationUnitCursor(tu);
        clang_visitChildren(
            root,
            [](CXCursor child, CXCursor, CXClientData clientData) {
                Payload* p = static_cast<Payload*>(clientData);
                if (clang_getCursorKind(child) != CXCursor_MacroExpansion)
                {
                    return CXChildVisit_Recurse;
                }

                CXCursor macroDef = clang_getCursorReferenced(child);
                if (clang_Cursor_isNull(macroDef)) return CXChildVisit_Continue;

                std::string macroKey;
                SourceOffsetLocation defLoc;
                if (FillSourceOffsetLocation(clang_getCursorLocation(macroDef), &defLoc))
                {
                    std::ostringstream oss;
                    oss << defLoc.filePath << ':' << defLoc.offset << ':'
                        << ToStdString(clang_getCursorSpelling(macroDef));
                    macroKey = oss.str();
                }
                else
                {
                    macroKey = ToStdString(clang_getCursorSpelling(macroDef));
                }

                bool macroAllows = false;
                std::map<std::string, bool>::iterator mit = p->macroAllowCache.find(macroKey);
                if (mit != p->macroAllowCache.end()) macroAllows = mit->second;
                else
                {
                    macroAllows = MacroDefinitionAllowsReflectFriend(p->tu, macroDef);
                    p->macroAllowCache[macroKey] = macroAllows;
                }
                if (!macroAllows) return CXChildVisit_Continue;

                SourceOffsetLocation beginInfo;
                SourceOffsetLocation endInfo;
                const CXSourceRange range = clang_getCursorExtent(child);
                if (!FillSourceOffsetLocation(clang_getRangeStart(range), &beginInfo)) return CXChildVisit_Continue;
                if (!FillSourceOffsetLocation(clang_getRangeEnd(range), &endInfo)) return CXChildVisit_Continue;
                if (beginInfo.filePath.empty() || beginInfo.filePath != endInfo.filePath) return CXChildVisit_Continue;

                ReflectFriendMacroExpansionRange outRange;
                outRange.filePath = beginInfo.filePath;
                outRange.beginOffset = beginInfo.offset;
                outRange.endOffset = endInfo.offset;
                outRange.macroName = ToStdString(clang_getCursorSpelling(child));
                p->ranges->push_back(outRange);
                return CXChildVisit_Continue;
            },
            &payload);

        gReflectFriendMacroRangeCache[tuKey] = ranges;
        return gReflectFriendMacroRangeCache[tuKey];
    }

    bool CursorOwningMacroExpansionAllowsReflectFriend(CXTranslationUnit tu, CXCursor recordCursor)
    {
        if (!tu) return false;
        const std::vector<ReflectFriendMacroExpansionRange>& ranges = GetReflectFriendMacroExpansionRanges(tu);
        if (ranges.empty()) return false;

        SourceOffsetLocation recordLoc;
        SourceOffsetLocation recordBegin;
        FillSourceOffsetLocation(clang_getCursorLocation(recordCursor), &recordLoc);
        FillSourceOffsetLocation(clang_getRangeStart(clang_getCursorExtent(recordCursor)), &recordBegin);

        for (std::size_t i = 0; i < ranges.size(); ++i)
        {
            if (SourceOffsetInRange(recordLoc, ranges[i]) || SourceOffsetInRange(recordBegin, ranges[i]))
            {
                std::cerr << "[friend kept] record macro-expansion contains WAVE_REFLECT_FRIEND macro=["
                          << ranges[i].macroName << "]\n";
                return true;
            }
        }
        return false;
    }

    bool FriendDeclAllowsReflectAccess(CXTranslationUnit tu, CXCursor cursor)
    {
        // A real FriendDecl may contain the expanded friend type name
        // (::wave::ReflectAccess) or, depending on macro handling, the macro token.
        std::string matched;
        return CursorTokensHaveReflectFriend(tu, cursor, true, &matched);
    }

    bool TextHasReflectFriendMacroOutsideCommentsAndLiterals(const std::string& text)
    {
        // Last-resort source fallback for cases where the target header contains
        // WAVE_REFLECT_FRIEND but libclang does not materialize a FriendDecl and
        // tokenization of the cursor extent is degraded by earlier parse errors
        // or by macro configuration.  We still avoid comments and literals so a
        // commented marker does not enable private reflection.
        enum State
        {
            Normal,
            LineComment,
            BlockComment,
            StringLiteral,
            CharLiteral
        } state = Normal;

        std::string ident;
        auto flushIdent = [&]() -> bool {
            if (ident.empty()) return false;
            const bool matched = IsReflectFriendIdentifier(ident, false);
            ident.clear();
            return matched;
        };

        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            const char n = (i + 1 < text.size()) ? text[i + 1] : '\0';

            switch (state)
            {
            case Normal:
                if (c == '/' && n == '/')
                {
                    if (flushIdent()) return true;
                    state = LineComment;
                    ++i;
                    continue;
                }
                if (c == '/' && n == '*')
                {
                    if (flushIdent()) return true;
                    state = BlockComment;
                    ++i;
                    continue;
                }
                if (c == '"')
                {
                    if (flushIdent()) return true;
                    state = StringLiteral;
                    continue;
                }
                if (c == '\'')
                {
                    if (flushIdent()) return true;
                    state = CharLiteral;
                    continue;
                }
                if ((c == '_') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (!ident.empty() && c >= '0' && c <= '9'))
                {
                    ident.push_back(c);
                    continue;
                }
                if (flushIdent()) return true;
                break;

            case LineComment:
                if (c == '\n' || c == '\r') state = Normal;
                break;

            case BlockComment:
                if (c == '*' && n == '/')
                {
                    state = Normal;
                    ++i;
                }
                break;

            case StringLiteral:
                if (c == '\\')
                {
                    ++i;
                    continue;
                }
                if (c == '"') state = Normal;
                break;

            case CharLiteral:
                if (c == '\\')
                {
                    ++i;
                    continue;
                }
                if (c == '\'') state = Normal;
                break;
            }
        }

        return flushIdent();
    }

    std::string GetCursorRawSourceTextFromFile(CXCursor cursor)
    {
        CXSourceRange range = clang_getCursorExtent(cursor);
        CXSourceLocation beginLoc = clang_getRangeStart(range);
        CXSourceLocation endLoc = clang_getRangeEnd(range);

        CXFile beginFile = NULL;
        CXFile endFile = NULL;
        unsigned beginOffset = 0;
        unsigned endOffset = 0;
        clang_getFileLocation(beginLoc, &beginFile, NULL, NULL, &beginOffset);
        clang_getFileLocation(endLoc, &endFile, NULL, NULL, &endOffset);
        if (beginFile == NULL || endFile == NULL) return std::string();
        const std::string beginPath = ToStdString(clang_getFileName(beginFile));
        const std::string endPath = ToStdString(clang_getFileName(endFile));
        if (beginPath.empty() || beginPath != endPath) return std::string();
        if (endOffset <= beginOffset) return std::string();

        std::ifstream in(beginPath.c_str(), std::ios::binary);
        if (!in) return std::string();
        in.seekg(0, std::ios::end);
        const std::streamoff fileSize = in.tellg();
        if (fileSize <= 0) return std::string();
        if (static_cast<unsigned long long>(beginOffset) >= static_cast<unsigned long long>(fileSize)) return std::string();
        if (static_cast<unsigned long long>(endOffset) > static_cast<unsigned long long>(fileSize)) return std::string();

        const std::size_t len = static_cast<std::size_t>(endOffset - beginOffset);
        std::string out(len, '\0');
        in.seekg(beginOffset, std::ios::beg);
        in.read(&out[0], static_cast<std::streamsize>(len));
        if (!in) return std::string();
        return out;
    }

    const std::string& GetSourceFileTextCached(const std::string& path)
    {
        static const std::string kEmpty;
        const std::string key = CanonicalSourcePathKey(path);
        std::map<std::string, std::string>::const_iterator it = gSourceFileTextCache.find(key);
        if (it != gSourceFileTextCache.end()) return it->second;

        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in)
        {
            gSourceFileTextCache[key] = std::string();
            return gSourceFileTextCache[key];
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        gSourceFileTextCache[key] = ss.str();
        return gSourceFileTextCache[key];
    }

    std::size_t FindNextCharOutsideCommentsAndLiterals(const std::string& text,
                                                       std::size_t start,
                                                       char target)
    {
        enum State
        {
            Normal,
            LineComment,
            BlockComment,
            StringLiteral,
            CharLiteral
        } state = Normal;

        for (std::size_t i = start; i < text.size(); ++i)
        {
            const char c = text[i];
            const char n = (i + 1 < text.size()) ? text[i + 1] : '\0';

            switch (state)
            {
            case Normal:
                if (c == target) return i;
                if (c == '/' && n == '/')
                {
                    state = LineComment;
                    ++i;
                    continue;
                }
                if (c == '/' && n == '*')
                {
                    state = BlockComment;
                    ++i;
                    continue;
                }
                if (c == '"')
                {
                    state = StringLiteral;
                    continue;
                }
                if (c == '\'')
                {
                    state = CharLiteral;
                    continue;
                }
                break;

            case LineComment:
                if (c == '\n' || c == '\r') state = Normal;
                break;

            case BlockComment:
                if (c == '*' && n == '/')
                {
                    state = Normal;
                    ++i;
                }
                break;

            case StringLiteral:
                if (c == '\\')
                {
                    ++i;
                    continue;
                }
                if (c == '"') state = Normal;
                break;

            case CharLiteral:
                if (c == '\\')
                {
                    ++i;
                    continue;
                }
                if (c == '\'') state = Normal;
                break;
            }
        }
        return std::string::npos;
    }

    std::size_t FindMatchingBraceOutsideCommentsAndLiterals(const std::string& text,
                                                            std::size_t openBrace)
    {
        if (openBrace >= text.size() || text[openBrace] != '{') return std::string::npos;

        enum State
        {
            Normal,
            LineComment,
            BlockComment,
            StringLiteral,
            CharLiteral
        } state = Normal;

        unsigned depth = 0;
        for (std::size_t i = openBrace; i < text.size(); ++i)
        {
            const char c = text[i];
            const char n = (i + 1 < text.size()) ? text[i + 1] : '\0';

            switch (state)
            {
            case Normal:
                if (c == '/' && n == '/')
                {
                    state = LineComment;
                    ++i;
                    continue;
                }
                if (c == '/' && n == '*')
                {
                    state = BlockComment;
                    ++i;
                    continue;
                }
                if (c == '"')
                {
                    state = StringLiteral;
                    continue;
                }
                if (c == '\'')
                {
                    state = CharLiteral;
                    continue;
                }
                if (c == '{')
                {
                    ++depth;
                    continue;
                }
                if (c == '}')
                {
                    if (depth == 0) return std::string::npos;
                    --depth;
                    if (depth == 0) return i;
                    continue;
                }
                break;

            case LineComment:
                if (c == '\n' || c == '\r') state = Normal;
                break;

            case BlockComment:
                if (c == '*' && n == '/')
                {
                    state = Normal;
                    ++i;
                }
                break;

            case StringLiteral:
                if (c == '\\')
                {
                    ++i;
                    continue;
                }
                if (c == '"') state = Normal;
                break;

            case CharLiteral:
                if (c == '\\')
                {
                    ++i;
                    continue;
                }
                if (c == '\'') state = Normal;
                break;
            }
        }
        return std::string::npos;
    }

    bool CursorBodySourceHasReflectFriend(CXCursor cursor, std::string* matchedToken)
    {
        if (matchedToken) matchedToken->clear();

        CXFile file = NULL;
        unsigned line = 0;
        unsigned column = 0;
        unsigned offset = 0;
        clang_getExpansionLocation(clang_getCursorLocation(cursor), &file, &line, &column, &offset);
        if (!file) return false;

        const std::string filePath = ToStdString(clang_getFileName(file));
        if (filePath.empty()) return false;

        const std::string& text = GetSourceFileTextCached(filePath);
        if (text.empty()) return false;
        if (static_cast<unsigned long long>(offset) >= static_cast<unsigned long long>(text.size())) return false;

        // Macro-wrapped class heads such as
        //   MY_CLASS_MACRO(Foo)
        //   {
        //       WAVE_REFLECT_FRIEND
        //       ...
        //   };
        // often give libclang a cursor extent that starts in the macro definition
        // rather than in the body file.  Instead of walking every macro expansion
        // in the TU, scan only the lexical class body immediately following the
        // cursor's expansion location.  This is O(class-body-size), cached per
        // cursor by CursorHasReflectAccessFriend(), and does not create temp files.
        const std::size_t start = static_cast<std::size_t>(offset);
        const std::size_t openBrace = FindNextCharOutsideCommentsAndLiterals(text, start, '{');
        if (openBrace == std::string::npos) return false;

        std::size_t closeBrace = FindMatchingBraceOutsideCommentsAndLiterals(text, openBrace);
        if (closeBrace == std::string::npos)
        {
            closeBrace = text.size() - 1;
        }
        if (closeBrace <= openBrace) return false;

        const std::size_t len = closeBrace - openBrace + 1;
        const std::string body = text.substr(openBrace, len);
        if (TextHasReflectFriendMacroOutsideCommentsAndLiterals(body))
        {
            if (matchedToken) *matchedToken = "WAVE_REFLECT_FRIEND";
            return true;
        }
        return false;
    }


    bool CursorHasReflectFriendMarkerDecl(CXCursor cursor)
    {
        struct Payload
        {
            bool found;
        } payload;
        payload.found = false;

        clang_visitChildren(
            cursor,
            [](CXCursor child, CXCursor, CXClientData clientData) {
                Payload* p = static_cast<Payload*>(clientData);
                if (p->found) return CXChildVisit_Break;

                const std::string name = ToStdString(clang_getCursorSpelling(child));
                if (name != kReflectFriendMarkerTypeAliasName)
                {
                    return CXChildVisit_Continue;
                }

                const CXCursorKind kind = clang_getCursorKind(child);
                if (kind == CXCursor_TypeAliasDecl || kind == CXCursor_TypedefDecl)
                {
                    p->found = true;
                    return CXChildVisit_Break;
                }

                // Compatibility only: older generated/test headers may still
                // contain the temporary static constexpr marker.  The current
                // WAVE_REFLECT_FRIEND macro emits a type alias, not a variable.
                if (kind == CXCursor_VarDecl)
                {
                    p->found = true;
                    return CXChildVisit_Break;
                }

                return CXChildVisit_Continue;
            },
            &payload);

        return payload.found;
    }

    bool CursorExpansionLocationIsFromMainFile(CXTranslationUnit tu, CXCursor cursor)
    {
        if (!tu) return clang_Location_isFromMainFile(clang_getCursorLocation(cursor)) != 0;

        CXFile file = NULL;
        unsigned line = 0;
        unsigned column = 0;
        unsigned offset = 0;
        clang_getExpansionLocation(clang_getCursorLocation(cursor), &file, &line, &column, &offset);
        if (!file) return clang_Location_isFromMainFile(clang_getCursorLocation(cursor)) != 0;

        CXSourceLocation expansionLoc = clang_getLocation(tu, file, line, column);
        return clang_Location_isFromMainFile(expansionLoc) != 0;
    }

    bool CursorHasReflectAccessFriend(CXTranslationUnit tu, CXCursor cursor)
    {
        const std::string cacheKey = CursorReflectFriendCacheKey(tu, cursor);
        std::map<std::string, bool>::const_iterator cached = gReflectFriendCursorCache.find(cacheKey);
        if (cached != gReflectFriendCursorCache.end()) return cached->second;

        bool result = false;

        // Fast path: rely on the real AST FriendDecl when libclang exposes it.
        // This covers the normal spelling:
        //   template <typename T> friend struct ::wave::ReflectAccess;
        struct Payload
        {
            CXTranslationUnit tu;
            bool found;
        } payload;
        payload.tu = tu;
        payload.found = false;
        clang_visitChildren(
            cursor,
            [](CXCursor child, CXCursor, CXClientData clientData) {
                Payload* p = static_cast<Payload*>(clientData);
                if (p->found) return CXChildVisit_Break;
                if (clang_getCursorKind(child) == CXCursor_FriendDecl &&
                    FriendDeclAllowsReflectAccess(p->tu, child))
                {
                    p->found = true;
                    return CXChildVisit_Break;
                }
                return CXChildVisit_Continue;
            },
            &payload);
        if (payload.found) result = true;

        // Marker-declaration fallback: WAVE_REFLECT_FRIEND also expands to a
        // class-scope type alias.  This is a pure AST child lookup, so it handles
        // macro-wrapped class heads without opening source files, scanning raw text,
        // or walking the TranslationUnit's macro expansion table.  The alias has
        // no storage and cannot be emitted as a reflected instance field.
        if (!result && CursorHasReflectFriendMarkerDecl(cursor)) result = true;

        // Cheap legacy fallback only: scan this record's own token range for the
        // explicit marker macro.  Do not walk the whole TranslationUnit's macro
        // expansion table and do not do source-path/range containment checks here.
        // That old path was O(records * macro-expansions) and dominated runtime in
        // macro-heavy projects.
        std::string matched;
        if (!result && CursorTokensHaveReflectFriend(tu, cursor, false, &matched)) result = true;

        gReflectFriendCursorCache[cacheKey] = result;
        return result;
    }

    bool RecordAllowsAccess(const RecordInfo& rec, CX_CXXAccessSpecifier access)
    {
        return rec.allowPrivateReflect || IsPublicAccess(access);
    }

    void PrintUsage()
    {
        std::cerr
            << "Usage:\n"
            << "  Single-header mode:\n"
            << "    ReflectGen <input_header> -o <output_header>\n"
            << "               [--header-include <header_for_generated_include>]\n"
            << "               [--main-file-only | --all-files]\n"
            << "               [-- <clang_parse_args...>]\n"
            << "\n"
            << "  Batch directory mode:\n"
            << "    ReflectGen --batch-dir <dir> [--batch-dir <dir2> ...] -o <output_dir>\n"
            << "               [--aggregate-header <project_reflect_auto.h>]\n"
            << "               [--recursive | --no-recursive]\n"
            << "               [--main-file-only | --all-files]\n"
            << "               [-- <clang_parse_args...>]\n"
            << "\n"
            << "  Batch directory-list mode:\n"
            << "    ReflectGen --batch-dir-list <dirs.txt> -o <output_dir>\n"
            << "               [--aggregate-header <project_reflect_auto.h>]\n"
            << "               [--main-file-only | --all-files]\n"
            << "               [-- <clang_parse_args...>]\n"
            << "\n"
            << "  Directory-list mode reads one directory per line, ignores blank/# or // comments,\n"
            << "  scans only the current directory level by default, generates one *_reflect_auto.h/.hpp\n"
            << "  per input header, then generates an umbrella header that includes all generated headers.\n"
            << "\n"
            << "  Optional Visual Studio project settings import:\n"
            << "    --vcxproj <project.vcxproj> [--config Release] [--platform x64]\n"
            << "  Optional whitelist source:\n"
            << "    --whitelist-from-header <aqcm.hpp>   use direct quoted includes in this header as reflection targets\n"
            << "    --extra-target-list <targets.txt>    supplement reflection targets from a txt file\n"
            << "    --reflect-root-class <ClassName>     emit only this class and recursively used member classes\n"
            << "    --root-class-list <classes.txt>      one root class per line; comments with # or // are ignored\n"
            << "    If omitted with --vcxproj, defaults to <vcxproj_dir>/inc/aqcm.hpp when it exists;\n"
            << "    <vcxproj_dir>/reflect_targets.txt is also used automatically when present.\n"
            << "  Optional diagnostics:\n"
            << "    --log-file <reflectgen_log.txt>\n"
            << "    --clangxx <path_to_clang++.exe>\n"
            << "    --debug-ast    print per-cursor AST logs and verbose clang/probe args\n"
            << "    --allow-errors continue generation even if libclang reports error/fatal diagnostics\n"
            << "    --no-expanded-friend-scan disable clang++ -E fallback friend detection\n"
            << "  This imports AdditionalIncludeDirectories, PreprocessorDefinitions,\n"
            << "  ForcedIncludeFiles and LanguageStandard as libclang arguments.\n"
            << "\n"
            << "  Logging without cmd.exe redirection:\n"
            << "    --log-file <reflectgen_log.txt>\n"
            << "  The path may contain environment variables such as %AQROOT% or $(AQROOT).\n";
    }

    bool ParseCommandLine(int argc, char** argv, Options& opt)
    {
        if (argc < 2)
        {
            PrintUsage();
            return false;
        }

        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

        std::size_t sep = args.size();
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--")
            {
                sep = i;
                break;
            }
        }

        std::vector<std::string> front(args.begin(), args.begin() + static_cast<std::ptrdiff_t>(sep));
        if (sep < args.size()) opt.clangArgs.assign(args.begin() + static_cast<std::ptrdiff_t>(sep + 1), args.end());

        for (std::size_t i = 0; i < front.size(); ++i)
        {
            const std::string& a = front[i];
            if (a == "-o")
            {
                if (i + 1 >= front.size()) return false;
                opt.outputHeader = front[++i];
            }
            else if (a == "--header-include")
            {
                if (i + 1 >= front.size()) return false;
                opt.headerInclude = front[++i];
            }
            else if (a == "--vcxproj" || a == "--project" || a == "--batch-vcxproj")
            {
                if (i + 1 >= front.size()) return false;
                opt.vcxprojPath = front[++i];
            }
            else if (a == "--config" || a == "--configuration")
            {
                if (i + 1 >= front.size()) return false;
                opt.configuration = front[++i];
            }
            else if (a == "--platform")
            {
                if (i + 1 >= front.size()) return false;
                opt.platform = front[++i];
            }
            else if (a == "--main-file-only")
            {
                opt.mainFileOnly = true;
            }
            else if (a == "--all-files")
            {
                opt.mainFileOnly = false;
            }
            else if (a == "--batch-dir")
            {
                if (i + 1 >= front.size()) return false;
                opt.batchMode = true;
                opt.batchDirs.push_back(front[++i]);
            }
            else if (a == "--batch-dir-list" || a == "--dir-list" || a == "--batch-list")
            {
                if (i + 1 >= front.size()) return false;
                opt.batchMode = true;
                opt.batchFromDirListFile = true;
                opt.batchDirListFile = front[++i];
            }
            else if (a == "--aggregate-header")
            {
                if (i + 1 >= front.size()) return false;
                opt.aggregateHeader = front[++i];
            }
            else if (a == "--whitelist-from-header" || a == "--include-whitelist-header" || a == "--batch-from-header")
            {
                if (i + 1 >= front.size()) return false;
                opt.batchMode = true;
                opt.includeWhitelistHeader = front[++i];
            }
            else if (a == "--extra-target-list" || a == "--reflect-target-list" || a == "--target-list" || a == "--targets-txt")
            {
                if (i + 1 >= front.size()) return false;
                opt.batchMode = true;
                opt.extraTargetListFile = front[++i];
            }
            else if (a == "--reflect-root-class" || a == "--root-class" || a == "--target-class" || a == "--class-root")
            {
                if (i + 1 >= front.size()) return false;
                opt.batchMode = true;
                opt.rootClassNames.push_back(front[++i]);
            }
            else if (a == "--root-class-list" || a == "--reflect-root-class-list" || a == "--class-list" || a == "--classes-txt")
            {
                if (i + 1 >= front.size()) return false;
                opt.batchMode = true;
                opt.rootClassListFile = front[++i];
            }
            else if (a == "--log-file" || a == "--log")
            {
                if (i + 1 >= front.size()) return false;
                opt.logFile = front[++i];
            }
            else if (a == "--clangxx" || a == "--clang++" || a == "--clang-compiler")
            {
                if (i + 1 >= front.size()) return false;
                opt.clangxxPath = front[++i];
            }
            else if (a == "--debug-ast" || a == "--verbose-ast")
            {
                opt.debugAst = true;
                gDebugAst = true;
            }
            else if (a == "--allow-errors" || a == "--allow-parse-errors")
            {
                opt.allowParseErrors = true;
            }
            else if (a == "--no-expanded-friend-scan")
            {
                opt.expandedFriendScan = false;
            }
            else if (a == "--expanded-friend-scan")
            {
                opt.expandedFriendScan = true;
            }
            else if (a == "--bool-storage-typedef" || a == "--bool-typedef")
            {
                if (i + 1 >= front.size()) return false;
                opt.boolStorageTypedefs.push_back(front[++i]);
            }
            else if (a == "--recursive")
            {
                opt.batchRecursive = true;
                opt.batchRecursiveSpecified = true;
            }
            else if (a == "--no-recursive")
            {
                opt.batchRecursive = false;
                opt.batchRecursiveSpecified = true;
            }
            else if (StartsWithDash(a))
            {
                return false;
            }
            else
            {
                if (!opt.inputHeader.empty()) return false;
                opt.inputHeader = a;
            }
        }

        if (opt.boolStorageTypedefs.empty()) {
            opt.boolStorageTypedefs.push_back("U01");
        }

        // Project shorthand: allow a minimal command line such as
        //   ReflectGen --vcxproj cmodel.vcxproj
        // In that case batch mode is implied, and the project-specific defaults
        // are filled after parsing when path helpers are available.
        if (!opt.vcxprojPath.empty() && opt.inputHeader.empty() && !opt.batchMode)
        {
            opt.batchMode = true;
        }

        if (opt.batchMode)
        {
            if (!opt.inputHeader.empty()) return false;
            if (opt.batchFromDirListFile)
            {
                if (opt.batchDirListFile.empty()) return false;
                if (!opt.batchRecursiveSpecified) opt.batchRecursive = false;
            }
            if (opt.batchDirs.empty() && opt.batchDirListFile.empty() && opt.includeWhitelistHeader.empty() && opt.vcxprojPath.empty()) return false;
            // outputHeader may be omitted for the common --vcxproj workflow;
            // ApplyHardcodedProjectDefaults() will fill <vcxproj_dir>/generated_reflect.
            if (opt.outputHeader.empty() && opt.vcxprojPath.empty()) return false;
            if (opt.aggregateHeader.empty()) opt.aggregateHeader = "project_reflect_auto.h";
        }
        else
        {
            if (opt.inputHeader.empty() || opt.outputHeader.empty()) return false;
            if (opt.headerInclude.empty()) opt.headerInclude = GetBaseName(opt.inputHeader);
        }

        if (opt.clangArgs.empty())
        {
            opt.clangArgs.push_back("-x");
            opt.clangArgs.push_back("c++");
        }
        return true;
    }

    bool IsRecordKind(CXCursorKind kind)
    {
        return kind == CXCursor_StructDecl || kind == CXCursor_ClassDecl;
    }

    std::string GetQualifiedName(CXCursor cursor)
    {
        std::vector<std::string> parts;
        for (CXCursor cur = cursor; !clang_Cursor_isNull(cur); cur = clang_getCursorSemanticParent(cur))
        {
            const CXCursorKind k = clang_getCursorKind(cur);
            if (k == CXCursor_TranslationUnit) break;
            if (k == CXCursor_Namespace || k == CXCursor_StructDecl || k == CXCursor_ClassDecl || k == CXCursor_ClassTemplate || k == CXCursor_TypedefDecl)
            {
                std::string name = ToStdString(clang_getCursorSpelling(cur));
                if (!name.empty()) parts.push_back(name);
            }
        }

        std::reverse(parts.begin(), parts.end());
        std::ostringstream oss;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            if (i) oss << "::";
            oss << parts[i];
        }
        return oss.str();
    }
    bool IsNonBusinessRecordCursor(CXCursor cursor);

    std::string CanonicalSourcePathKey(std::string path)
    {
        path = NormalizePathSlashes(path);
        std::map<std::string, std::string>::const_iterator it = gCanonicalSourcePathCache.find(path);
        if (it != gCanonicalSourcePathCache.end()) return it->second;

        std::string out = path;
        if (!out.empty()) out = MakeAbsoluteLexicalPath(out);
        out = NormalizePathSlashes(out);
#ifdef _WIN32
        out = ToLowerAscii(out);
#endif
        gCanonicalSourcePathCache[path] = out;
        return out;
    }

    std::string GetCursorSourceFilePath(CXCursor cursor)
    {
        CXSourceLocation loc = clang_getCursorLocation(cursor);
        CXFile file = NULL;
        unsigned line = 0;
        unsigned column = 0;
        unsigned offset = 0;

        // Prefer expansion location. Macro-generated fields/classes should be attributed
        // to the header where the macro is invoked, not to the helper header where the
        // macro body is defined. Otherwise private fields generated by class macros can be
        // incorrectly filtered out by the source whitelist.
        clang_getExpansionLocation(loc, &file, &line, &column, &offset);
        if (!file)
        {
            clang_getSpellingLocation(loc, &file, &line, &column, &offset);
        }
        if (!file) return std::string();
        return CanonicalSourcePathKey(ToStdString(clang_getFileName(file)));
    }

    bool CursorPassesSourceWhitelist(CXCursor cursor, const CollectContext& ctx)
    {
        if (!ctx.useFileWhitelist) return true;
        const std::string source = GetCursorSourceFilePath(cursor);
        return !source.empty() && ctx.sourceFileWhitelist.find(source) != ctx.sourceFileWhitelist.end();
    }

    bool IsRecordLikeCursorKind(CXCursorKind kind)
    {
        return kind == CXCursor_StructDecl ||
            kind == CXCursor_ClassDecl ||
            kind == CXCursor_ClassTemplate;
    }

    bool IsNestedInsideRecordLike(CXCursor cursor, CXCursor* parentOut = NULL)
    {
        CXCursor parent = clang_getCursorSemanticParent(cursor);
        if (clang_Cursor_isNull(parent)) return false;
        const CXCursorKind pk = clang_getCursorKind(parent);
        if (!IsRecordLikeCursorKind(pk)) return false;
        if (parentOut) *parentOut = parent;
        return true;
    }

    bool IsUnionCursorKind(CXCursorKind kind)
    {
        return kind == CXCursor_UnionDecl;
    }

    bool IsInsideUnionByParentChain(CXCursor cursor, bool semantic)
    {
        CXCursor cur = semantic ? clang_getCursorSemanticParent(cursor) : clang_getCursorLexicalParent(cursor);
        for (; !clang_Cursor_isNull(cur); cur = semantic ? clang_getCursorSemanticParent(cur) : clang_getCursorLexicalParent(cur))
        {
            const CXCursorKind k = clang_getCursorKind(cur);
            if (k == CXCursor_TranslationUnit) break;
            if (k == CXCursor_UnionDecl) return true;
        }
        return false;
    }

    bool IsInsideUnion(CXCursor cursor)
    {
        // Anonymous union fields may have the containing class/struct as their
        // semantic parent, while their lexical parent still points into the
        // anonymous union.  Check both chains so injected union members are not
        // emitted as if they were normal obj->field members.
        return IsInsideUnionByParentChain(cursor, true) ||
            IsInsideUnionByParentChain(cursor, false);
    }

    bool IsUnionTypeCursor(CXCursor cursor)
    {
        if (clang_Cursor_isNull(cursor)) return false;
        const CXCursorKind k = clang_getCursorKind(cursor);
        return k == CXCursor_UnionDecl;
    }

    bool IsUnionFieldCursor(CXCursor fieldCursor)
    {
        if (clang_getCursorKind(fieldCursor) != CXCursor_FieldDecl) return false;
        CXType fieldType = clang_getCursorType(fieldCursor);
        CXCursor typeDecl = clang_getTypeDeclaration(fieldType);
        if (!clang_Cursor_isNull(typeDecl))
        {
            CXCursor def = clang_getCursorDefinition(typeDecl);
            if (!clang_Cursor_isNull(def)) typeDecl = def;
            if (IsUnionTypeCursor(typeDecl)) return true;
        }
        return false;
    }

    bool CursorAccessPathIsPublicOrTopLevel(CXCursor cursor)
    {
        // A nested type is externally nameable only if every component in its
        // qualified path is public.  Example:
        //
        //   class A {
        //   private:
        //       struct B { public: struct C {}; };
        //   };
        //
        // C itself is public inside B, but A::B::C is still inaccessible because
        // B is private in A.  Checking only clang_getCXXAccessSpecifier(C) is not
        // enough; we must walk C -> B -> A and validate every edge.
        CXCursor cur = cursor;
        while (!clang_Cursor_isNull(cur))
        {
            CXCursor parent = clang_getCursorSemanticParent(cur);
            if (clang_Cursor_isNull(parent)) break;

            const CXCursorKind pk = clang_getCursorKind(parent);
            if (pk == CXCursor_TranslationUnit || pk == CXCursor_Namespace) break;

            if (IsRecordLikeCursorKind(pk))
            {
                const bool parentIsStruct = (pk == CXCursor_StructDecl);
                const CX_CXXAccessSpecifier access = NormalizeAccess(clang_getCXXAccessSpecifier(cur), parentIsStruct);
                if (access != CX_CXXPublic) return false;
            }

            cur = parent;
        }
        return true;
    }

    bool CursorAccessIsPublicOrTopLevel(CXCursor cursor)
    {
        return CursorAccessPathIsPublicOrTopLevel(cursor);
    }
    bool CursorAccessPathIsReflectAccessible(CXTranslationUnit tu, CXCursor cursor)
    {
        CXCursor cur = cursor;
        while (!clang_Cursor_isNull(cur))
        {
            CXCursor parent = clang_getCursorSemanticParent(cur);
            if (clang_Cursor_isNull(parent)) break;

            const CXCursorKind pk = clang_getCursorKind(parent);
            if (pk == CXCursor_TranslationUnit || pk == CXCursor_Namespace) break;

            if (IsRecordLikeCursorKind(pk))
            {
                const bool parentIsStruct = (pk == CXCursor_StructDecl);
                const CX_CXXAccessSpecifier access = NormalizeAccess(clang_getCXXAccessSpecifier(cur), parentIsStruct);
                if (access != CX_CXXPublic && !CursorHasReflectAccessFriend(tu, parent))
                {
                    return false;
                }
            }

            cur = parent;
        }
        return true;
    }

    bool CursorAccessIsReflectAccessible(CXTranslationUnit tu, CXCursor cursor)
    {
        return CursorAccessPathIsReflectAccessible(tu, cursor);
    }


    CXType StripTypeForAccessPathCheck(CXType type)
    {
        CXType t = clang_getCanonicalType(type);
        bool changed = true;
        while (changed)
        {
            changed = false;
            switch (t.kind)
            {
            case CXType_Pointer:
            case CXType_LValueReference:
            case CXType_RValueReference:
                t = clang_getCanonicalType(clang_getPointeeType(t));
                changed = true;
                break;
            case CXType_ConstantArray:
            case CXType_IncompleteArray:
            case CXType_VariableArray:
            case CXType_DependentSizedArray:
                t = clang_getCanonicalType(clang_getArrayElementType(t));
                changed = true;
                break;
            default:
                break;
            }
        }
        return t;
    }

    bool FieldTypeAccessPathIsReflectAccessible(CXTranslationUnit tu, CXCursor fieldCursor)
    {
        if (clang_getCursorKind(fieldCursor) != CXCursor_FieldDecl) return true;
        CXType fieldType = StripTypeForAccessPathCheck(clang_getCursorType(fieldCursor));
        CXCursor typeDecl = clang_getTypeDeclaration(fieldType);
        if (clang_Cursor_isNull(typeDecl)) return true;
        CXCursor def = clang_getCursorDefinition(typeDecl);
        if (!clang_Cursor_isNull(def)) typeDecl = def;
        const CXCursorKind k = clang_getCursorKind(typeDecl);
        if (!IsRecordKind(k) && k != CXCursor_ClassTemplate) return true;
        return CursorAccessPathIsReflectAccessible(tu, typeDecl);
    }

    bool IsBuiltinOrNonRecordLookupKey(const std::string& key);

    bool IsDependentOrTemplateParamFieldType(CXCursor fieldCursor)
    {
        if (clang_getCursorKind(fieldCursor) != CXCursor_FieldDecl) return false;
        CXType t = clang_getCursorType(fieldCursor);
        CXType stripped = StripTypeForAccessPathCheck(t);
        CXCursor decl = clang_getTypeDeclaration(stripped);
        if (!clang_Cursor_isNull(decl))
        {
            const CXCursorKind dk = clang_getCursorKind(decl);
            if (dk == CXCursor_TemplateTypeParameter ||
                dk == CXCursor_NonTypeTemplateParameter ||
                dk == CXCursor_TemplateTemplateParameter)
            {
                return true;
            }
        }
        const std::string spelling = Trim(ToStdString(clang_getTypeSpelling(t)));
        const std::string canonical = Trim(ToStdString(clang_getTypeSpelling(clang_getCanonicalType(t))));
        if (spelling.empty()) return false;
        // Common libclang spellings for dependent fields are T, const T&,
        // typename X::Y, or type-parameter-* in canonical spelling. These must
        // not suppress field emission; the instantiated C++ type is resolved by
        // the compiler when generated visitor code takes &(obj->field).
        if (spelling.find("type-parameter-") != std::string::npos ||
            canonical.find("type-parameter-") != std::string::npos ||
            spelling.find("typename ") != std::string::npos)
        {
            return true;
        }
        // A single identifier without namespace/templates/pointer syntax is often
        // a template parameter. Treat it as dependent only when we are in a template
        // context; this helper is deliberately permissive only for access checks.
        bool simpleIdentifier = true;
        for (std::size_t i = 0; i < spelling.size(); ++i)
        {
            const char c = spelling[i];
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            {
                simpleIdentifier = false;
                break;
            }
        }
        return simpleIdentifier && !IsBuiltinOrNonRecordLookupKey(CanonicalRecordLookupKey(spelling));
    }

    bool RecordHasExplicitTagNameByTokens(CXTranslationUnit tu, CXCursor cursor)
    {
        // libclang can expose anonymous field records with synthetic spellings
        // such as "inInstr32Field_NonCtrl". Those spellings are not valid C++
        // type names. A record is emitted from its own cursor only when its
        // declaration has a real tag name after the class/struct keyword.
        // C-style typedef anonymous records are handled separately from the
        // TypedefDecl cursor.
        if (!tu) return true;

        CXSourceRange range = clang_getCursorExtent(cursor);
        CXToken* tokens = NULL;
        unsigned tokenCount = 0;
        clang_tokenize(tu, range, &tokens, &tokenCount);
        if (!tokens || tokenCount == 0)
        {
            if (tokens) clang_disposeTokens(tu, tokens, tokenCount);
            return true;
        }

        bool sawKeyword = false;
        bool explicitTag = true;
        for (unsigned i = 0; i < tokenCount; ++i)
        {
            const std::string tok = ToStdString(clang_getTokenSpelling(tu, tokens[i]));
            if (!sawKeyword)
            {
                if (tok == "struct" || tok == "class") sawKeyword = true;
                continue;
            }

            if (tok.empty()) continue;
            if (tok == "{")
            {
                explicitTag = false;
                break;
            }
            if (tok == ":")
            {
                explicitTag = false;
                break;
            }

            // Any token before the opening brace usually denotes an explicit tag
            // or a declaration attribute/macro followed by a tag. Treat it as
            // usable rather than aggressively dropping named project classes.
            explicitTag = true;
            break;
        }

        clang_disposeTokens(tu, tokens, tokenCount);
        return explicitTag;
    }

    std::string GetCursorKindName(CXCursorKind kind)
    {
        return ToStdString(clang_getCursorKindSpelling(kind));
    }

    std::string GetCursorLocString(CXCursor cursor)
    {
        CXSourceLocation loc = clang_getCursorLocation(cursor);
        CXFile file = NULL;
        unsigned line = 0;
        unsigned column = 0;
        unsigned offset = 0;
        clang_getExpansionLocation(loc, &file, &line, &column, &offset);

        std::ostringstream oss;
        if (file) oss << ToStdString(clang_getFileName(file));
        else oss << "<no-file>";
        oss << ":" << line << ":" << column;
        return oss.str();
    }

    std::string GetCursorSpellingLocString(CXCursor cursor)
    {
        CXSourceLocation loc = clang_getCursorLocation(cursor);
        CXFile file = NULL;
        unsigned line = 0;
        unsigned column = 0;
        unsigned offset = 0;
        clang_getSpellingLocation(loc, &file, &line, &column, &offset);

        std::ostringstream oss;
        if (file) oss << ToStdString(clang_getFileName(file));
        else oss << "<no-file>";
        oss << ":" << line << ":" << column;
        return oss.str();
    }

    std::string GetCursorDisplayName(CXCursor cursor)
    {
        return ToStdString(clang_getCursorDisplayName(cursor));
    }

    std::string GetCursorTypeNameForDebug(CXCursor cursor)
    {
        CXType t = clang_getCursorType(cursor);
        if (t.kind == CXType_Invalid) return std::string();
        return ToStdString(clang_getTypeSpelling(t));
    }

    std::string GetCursorUsrForDebug(CXCursor cursor)
    {
        return ToStdString(clang_getCursorUSR(cursor));
    }

    bool LooksLikeLibclangAnonymousName(const std::string& s)
    {
        return s.find("anonymous") != std::string::npos ||
            s.find("unnamed") != std::string::npos ||
            s.find("(anonymous") != std::string::npos;
    }

    void PrintCursorDebugLine(const char* tag, CXCursor cursor, const std::string& extra)
    {
        if (!gDebugAst) return;
        const CXCursorKind kind = clang_getCursorKind(cursor);
        const std::string spelling = ToStdString(clang_getCursorSpelling(cursor));
        const std::string display = GetCursorDisplayName(cursor);
        const std::string qname = GetQualifiedName(cursor);
        const std::string typeName = GetCursorTypeNameForDebug(cursor);
        const std::string usr = GetCursorUsrForDebug(cursor);

        std::cerr << tag
            << " kind=" << GetCursorKindName(kind)
            << " spelling=[" << spelling << "]"
            << " display=[" << display << "]"
            << " qname=[" << qname << "]";
        if (!typeName.empty()) std::cerr << " type=[" << typeName << "]";
        if (!usr.empty()) std::cerr << " usr=[" << usr << "]";
        std::cerr << " loc=" << GetCursorLocString(cursor)
            << " spellingLoc=" << GetCursorSpellingLocString(cursor)
            << " fromMain=" << (clang_Location_isFromMainFile(clang_getCursorLocation(cursor)) ? "yes" : "no");
        if (!extra.empty()) std::cerr << " " << extra;
        std::cerr << "\n";
    }

    std::string GetRecordSkipReason(CXCursor cursor, const CollectContext& ctx)
    {
        const CXCursorKind kind = clang_getCursorKind(cursor);
        if (!IsRecordKind(kind)) return "not class/struct cursor";
        if (clang_getCursorKind(clang_getCursorSemanticParent(cursor)) == CXCursor_ClassTemplate)
        {
            return "template backing record; handled by ClassTemplate cursor";
        }
        if (!clang_isCursorDefinition(cursor)) return "not a definition";
        if (IsInsideUnion(cursor)) return "record declared inside union is skipped";
        if (!CursorAccessIsReflectAccessible(ctx.tu, cursor)) return "nested record has non-public access path and no ReflectAccess friend";
        if (!RecordHasExplicitTagNameByTokens(ctx.tu, cursor)) return "tagless anonymous/synthetic record; handled only through typedef alias";

        const std::string spelling = ToStdString(clang_getCursorSpelling(cursor));
        if (spelling.empty()) return "empty cursor spelling / unnamed record";
        if (LooksLikeLibclangAnonymousName(spelling)) return "spelling looks anonymous/unnamed";

        if (!CursorPassesSourceWhitelist(cursor, ctx))
        {
            return "not in batch source whitelist";
        }
        if (ctx.mainFileOnly && !CursorExpansionLocationIsFromMainFile(ctx.tu, cursor))
        {
            return "not from main file under --main-file-only";
        }
        if (IsNonBusinessRecordCursor(cursor)) return "non-business record filtered by reflect/sc_core/vsip rules";
        return std::string();
    }

    std::string GetTemplateRecordSkipReason(CXCursor cursor, const CollectContext& ctx)
    {
        if (clang_getCursorKind(cursor) != CXCursor_ClassTemplate) return "not ClassTemplate cursor";
        if (IsInsideUnion(cursor)) return "class template declared inside union is skipped";
        if (!CursorAccessIsReflectAccessible(ctx.tu, cursor)) return "nested class template has non-public access path and no ReflectAccess friend";
        if (!CursorPassesSourceWhitelist(cursor, ctx))
        {
            return "not in batch source whitelist";
        }
        if (ctx.mainFileOnly && !CursorExpansionLocationIsFromMainFile(ctx.tu, cursor))
        {
            return "not from main file under --main-file-only";
        }
        const std::string spelling = ToStdString(clang_getCursorSpelling(cursor));
        if (spelling.empty()) return "empty class-template spelling";
        if (LooksLikeLibclangAnonymousName(spelling)) return "template spelling looks anonymous/unnamed";
        if (IsNonBusinessRecordCursor(cursor)) return "non-business template filtered by reflect/sc_core/vsip rules";
        return std::string();
    }

    bool ShouldPrintSkipReason(const std::string& reason)
    {
        // Include headers can contain thousands of STL/SystemC records. In the
        // common --main-file-only mode they are intentionally ignored; counting
        // them is useful, printing every one of them is not. Main-file skips and
        // semantic skips still get full cursor diagnostics.
        return reason != "not from main file under --main-file-only" &&
            reason != "not in batch source whitelist";
    }

    void PrintRecordCollectionSummary(const Options& opt, const CollectContext& ctx)
    {
        std::cerr << "[collect summary] input=" << opt.inputHeader
            << " mainFileOnly=" << (ctx.mainFileOnly ? "yes" : "no")
            << " templateCandidates=" << ctx.classTemplateCandidates
            << " templateCollected=" << ctx.classTemplateCollected
            << " templateSkipped=" << ctx.classTemplateSkipped
            << " recordCandidates=" << ctx.recordCandidates
            << " recordCollected=" << ctx.recordCollected
            << " recordSkipped=" << ctx.recordSkipped
            << " recordDuplicates=" << ctx.recordDuplicates
            << " basesKept=" << ctx.basesKept
            << " basesSkippedSystemC=" << ctx.basesSkippedSystemC
            << " fieldsKept=" << ctx.fieldsKept
            << " fieldsSkippedEmptyName=" << ctx.fieldsSkippedEmptyName
            << " fieldsSkippedSystemC=" << ctx.fieldsSkippedSystemC
            << " emittedRecords=" << ctx.records.size()
            << "\n";

        if (ctx.records.empty())
        {
            std::cerr << "[collect warning] No reflectable record emitted for input=" << opt.inputHeader << "\n";
            if (ctx.classTemplateCandidates == 0 && ctx.recordCandidates == 0)
            {
                std::cerr << "[collect hint] libclang did not expose any ClassTemplate/ClassDecl/StructDecl cursor while visiting the TranslationUnit. "
                    << "Check include guards, preprocessor conditions, forced includes/PCH, and whether the target class is hidden behind a macro that was not expanded.\n";
            }
            else
            {
                std::cerr << "[collect hint] libclang saw candidate records/templates, but ReflectGen skipped or deduplicated all of them. "
                    << "Re-run with --debug-ast to print [record skipped] and [template skipped] cursor details.\n";
            }
        }
    }

    bool IsUnderNamespaceNamed(CXCursor cursor, const std::string& namespaceName)
    {
        for (CXCursor cur = clang_getCursorSemanticParent(cursor); !clang_Cursor_isNull(cur); cur = clang_getCursorSemanticParent(cur))
        {
            const CXCursorKind k = clang_getCursorKind(cur);
            if (k == CXCursor_TranslationUnit) break;
            if (k == CXCursor_Namespace)
            {
                if (ToStdString(clang_getCursorSpelling(cur)) == namespaceName)
                {
                    return true;
                }
            }
        }
        return false;
    }

    bool IsVsipPortRecordCursor(CXCursor cursor)
    {
        const std::string name = ToStdString(clang_getCursorSpelling(cursor));
        return name == "vsipIN" || name == "vsipOUT" || name == "vsipINOUT" ||
               name == "vsiiIN" || name == "vsiiOUT" || name == "vsiiINOUT";
    }

    bool IsNonBusinessRecordCursor(CXCursor cursor)
    {
        // Skip infrastructure / wrapper record definitions themselves.
        // This only controls whether ReflectGen emits reflected_visitor<T> for
        // a type definition. FieldDecl members are filtered separately so
        // whitelisted value-source fields such as sc_core::sc_in<T>, exact
        // vsipIN<T> ports, and vsiiIN<T>-derived channel/interface classes can
        // still be emitted and sampled by wave_runtime.
        if (IsUnderNamespaceNamed(cursor, "reflect")) return true;
        if (IsUnderNamespaceNamed(cursor, "wave")) return true;
        if (IsUnderNamespaceNamed(cursor, "sc_core")) return true;
        if (IsVsipPortRecordCursor(cursor)) return true;

        const std::string qn = GetQualifiedName(cursor);
        if (qn.compare(0, 9, "reflect::") == 0) return true;
        if (qn.compare(0, 11, "::reflect::") == 0) return true;
        if (qn.compare(0, 6, "wave::") == 0) return true;
        if (qn.compare(0, 8, "::wave::") == 0) return true;
        if (qn.compare(0, 9, "sc_core::") == 0) return true;
        if (qn.compare(0, 11, "::sc_core::") == 0) return true;
        return false;
    }

    std::string NormalizeTypeLookupKey(const std::string& typeName)
    {
        std::string key = CanonicalRecordLookupKey(typeName);
        key = Trim(key);
        while (ConsumePrefix(key, "::")) key = Trim(key);
        return key;
    }

    bool IsScCoreTypeName(const std::string& typeName)
    {
        const std::string key = NormalizeTypeLookupKey(typeName);
        return key.compare(0, 9, "sc_core::") == 0 || key == "sc_core";
    }


    CXType StripCvPointerReferenceArrayType(CXType type)
    {
        bool changed = true;
        while (changed)
        {
            changed = false;
            CXType canonical = clang_getCanonicalType(type);
            const CXTypeKind k = type.kind;
            if (k == CXType_Pointer || k == CXType_LValueReference || k == CXType_RValueReference)
            {
                type = clang_getPointeeType(type);
                changed = true;
                continue;
            }
            if (k == CXType_ConstantArray || k == CXType_IncompleteArray || k == CXType_VariableArray || k == CXType_DependentSizedArray)
            {
                type = clang_getArrayElementType(type);
                changed = true;
                continue;
            }
            if (k == CXType_Typedef || k == CXType_Elaborated)
            {
                type = canonical;
                changed = true;
                continue;
            }
        }
        return type;
    }

    std::string GetTypeDeclarationQualifiedName(CXType type)
    {
        CXType stripped = StripCvPointerReferenceArrayType(type);
        CXCursor decl = clang_getTypeDeclaration(stripped);
        if (clang_Cursor_isNull(decl))
        {
            CXType canonical = clang_getCanonicalType(stripped);
            decl = clang_getTypeDeclaration(canonical);
        }
        if (clang_Cursor_isNull(decl)) return std::string();
        return GetQualifiedName(decl);
    }

    bool IsWhitelistedSystemCMemberType(const std::string& typeName)
    {
        const std::string key = NormalizeTypeLookupKey(typeName);

        // These SystemC wrappers are value sources / containers that wave_runtime
        // knows how to sample through read() or indexed expansion. Keep the
        // member itself; skip other sc_core infrastructure fields.
        return key == "sc_core::sc_in" ||
            key == "sc_core::sc_out" ||
            key == "sc_core::sc_inout" ||
            key == "sc_core::sc_signal" ||
            key == "sc_core::sc_buffer" ||
            key == "sc_core::sc_vector" ||
            key == "sc_core::sc_clock";
    }

    bool ShouldSkipFieldType(const std::string& typeName)
    {
        // sc_core internals such as sc_event/sc_object/sc_module/
        // sc_process_handle/sc_port_base/etc. are infrastructure, not business
        // state. Do not emit on_ptr() for them.
        return IsScCoreTypeName(typeName) && !IsWhitelistedSystemCMemberType(typeName);
    }

    bool IsSystemCBaseInfo(const BaseInfo& b)
    {
        return IsScCoreTypeName(b.typeName) || IsScCoreTypeName(b.qualifiedTypeName);
    }


    bool IsExplicitTemplateSpecialization(CXCursor cursor)
    {
        if (!IsRecordKind(clang_getCursorKind(cursor))) return false;
        if (!clang_isCursorDefinition(cursor)) return false;
        const CXCursor specialized = clang_getSpecializedCursorTemplate(cursor);
        if (clang_Cursor_isNull(specialized)) return false;
        const CXCursorKind sk = clang_getCursorKind(specialized);
        return sk == CXCursor_ClassTemplate || IsRecordKind(sk);
    }

    bool ShouldSkipRecord(CXCursor cursor, const CollectContext& ctx)
    {
        return !GetRecordSkipReason(cursor, ctx).empty();
    }

    void CollectRecordBody(CXTranslationUnit tu, CXCursor recordCursor, RecordInfo& rec, CollectContext* ctx)
    {
        const bool friendAllowedBeforeFields = CursorHasReflectAccessFriend(tu, recordCursor);
        if (friendAllowedBeforeFields)
        {
            rec.allowPrivateReflect = true;
            PrintCursorDebugLine("[friend kept]", recordCursor,
                "owner=[" + rec.qualifiedName + "] reason=record-pre-scan-or-macro allowPrivateReflect=yes text=[" +
                ShortenForDebugLog(GetCursorSourceText(tu, recordCursor)) + "]");
        }
        else
        {
            PrintCursorDebugLine("[friend not found]", recordCursor,
                "owner=[" + rec.qualifiedName + "] allowPrivateReflect=no text=[" +
                ShortenForDebugLog(GetCursorSourceText(tu, recordCursor)) + "]");
        }

        struct Payload
        {
            CXTranslationUnit tu;
            CXCursor recordCursor;
            RecordInfo* out;
            CollectContext* ctx;
        } payload{ tu, recordCursor, &rec, ctx };

        clang_visitChildren(
            recordCursor,
            [](CXCursor child, CXCursor, CXClientData clientData) {
                Payload* payload = static_cast<Payload*>(clientData);
                RecordInfo* out = payload->out;
                CollectContext* ctx = payload->ctx;
                const CXCursorKind kind = clang_getCursorKind(child);

                if (kind == CXCursor_CXXBaseSpecifier)
                {
                    BaseInfo b;
                    CXType baseType = clang_getCursorType(child);
                    b.typeName = ToStdString(clang_getTypeSpelling(baseType));
                    b.canonicalTypeName = ToStdString(clang_getTypeSpelling(clang_getCanonicalType(baseType)));
                    CXCursor baseDecl = clang_getTypeDeclaration(baseType);
                    if (!clang_Cursor_isNull(baseDecl)) b.qualifiedTypeName = GetQualifiedName(baseDecl);
                    b.access = NormalizeAccess(clang_getCXXAccessSpecifier(child), out->isStruct);
                    if (!IsSystemCBaseInfo(b) && !b.typeName.empty())
                    {
                        out->bases.push_back(b);
                        if (ctx) ++ctx->basesKept;
                        PrintCursorDebugLine("[base kept]", child, "owner=[" + out->qualifiedName + "] type=[" + b.typeName + "] qtype=[" + b.qualifiedTypeName + "]");
                    }
                    else
                    {
                        if (ctx) ++ctx->basesSkippedSystemC;
                        PrintCursorDebugLine("[base skipped]", child, "reason=systemc-or-empty owner=[" + out->qualifiedName + "] type=[" + b.typeName + "] qtype=[" + b.qualifiedTypeName + "]");
                    }
                    return CXChildVisit_Continue;
                }

                if (kind == CXCursor_FieldDecl)
                {
                    if (IsInsideUnion(child) || IsUnionFieldCursor(child))
                    {
                        if (ctx) ++ctx->fieldsSkippedSystemC;
                        PrintCursorDebugLine("[field skipped]", child, "reason=union-field-or-inside-union owner=[" + out->qualifiedName + "]");
                        return CXChildVisit_Continue;
                    }

                    CXCursor semParent = clang_getCursorSemanticParent(child);
                    if (!clang_Cursor_isNull(semParent) &&
                        !clang_equalCursors(semParent, payload->recordCursor))
                    {
                        // Do not emit fields that libclang exposes through anonymous
                        // injected records but that are not addressable as obj->field
                        // on the current record.
                        if (ctx) ++ctx->fieldsSkippedEmptyName;
                        PrintCursorDebugLine("[field skipped]", child, "reason=semantic-parent-not-current-record owner=[" + out->qualifiedName + "]");
                        return CXChildVisit_Continue;
                    }

                    FieldInfo f;
                    f.name = ToStdString(clang_getCursorSpelling(child));
                    if (f.name.empty())
                    {
                        if (ctx) ++ctx->fieldsSkippedEmptyName;
                        PrintCursorDebugLine("[field skipped]", child, "reason=empty-name owner=[" + out->qualifiedName + "]");
                        return CXChildVisit_Continue;
                    }
                    CXType fieldType = clang_getCursorType(child);
                    f.typeName = ToStdString(clang_getTypeSpelling(fieldType));
                    f.canonicalTypeName = ToStdString(clang_getTypeSpelling(clang_getCanonicalType(fieldType)));
                    f.declQualifiedName = GetTypeDeclarationQualifiedName(fieldType);
                    if (ShouldSkipFieldType(f.typeName))
                    {
                        if (ctx) ++ctx->fieldsSkippedSystemC;
                        PrintCursorDebugLine("[field skipped]", child, "reason=systemc-non-whitelist owner=[" + out->qualifiedName + "] field=[" + f.name + "] type=[" + f.typeName + "]");
                        return CXChildVisit_Continue;
                    }
                    if (!FieldTypeAccessPathIsReflectAccessible(payload->tu, child))
                    {
                        if (IsDependentOrTemplateParamFieldType(child))
                        {
                            PrintCursorDebugLine("[field kept]", child, "reason=dependent-field-type-access-check-deferred owner=[" + out->qualifiedName + "] field=[" + f.name + "] type=[" + f.typeName + "] canonical=[" + f.canonicalTypeName + "]");
                        }
                        else
                        {
                            if (ctx) ++ctx->fieldsSkippedEmptyName;
                            PrintCursorDebugLine("[field skipped]", child, "reason=field-type-has-non-public-access-path owner=[" + out->qualifiedName + "] field=[" + f.name + "] type=[" + f.typeName + "] canonical=[" + f.canonicalTypeName + "]");
                            return CXChildVisit_Continue;
                        }
                    }
                    f.access = NormalizeAccess(clang_getCXXAccessSpecifier(child), out->isStruct);
                    f.isBitField = clang_Cursor_isBitField(child) != 0;
                    if (f.isBitField) f.bitWidth = clang_getFieldDeclBitWidth(child);
                    out->fields.push_back(f);
                    if (ctx) ++ctx->fieldsKept;
                    PrintCursorDebugLine("[field kept]", child, "owner=[" + out->qualifiedName + "] field=[" + f.name + "] type=[" + f.typeName + "] access=" + std::to_string(static_cast<int>(f.access)) + " allowPrivateReflect=" + (out->allowPrivateReflect ? "yes" : "no"));
                    return CXChildVisit_Continue;
                }

                if (kind == CXCursor_FriendDecl)
                {
                    if (FriendDeclAllowsReflectAccess(payload->tu, child))
                    {
                        out->allowPrivateReflect = true;
                        PrintCursorDebugLine("[friend kept]", child, "owner=[" + out->qualifiedName + "] reason=friend-decl-text text=[" + ShortenForDebugLog(GetCursorSourceText(payload->tu, child)) + "]");
                    }
                    return CXChildVisit_Continue;
                }

                return CXChildVisit_Continue;
            },
            &payload);
    }

    bool CollectTemplateRecord(CXTranslationUnit tu, CXCursor cursor, CollectContext& ctx, RecordInfo& rec)
    {
        const std::string skipReason = GetTemplateRecordSkipReason(cursor, ctx);
        if (!skipReason.empty())
        {
            ++ctx.classTemplateSkipped;
            if (ShouldPrintSkipReason(skipReason))
            {
                PrintCursorDebugLine("[template skipped]", cursor, "reason=" + skipReason);
            }
            return false;
        }

        PrintCursorDebugLine("[template candidate]", cursor, "status=collecting");

        struct Payload
        {
            CXTranslationUnit tu;
            RecordInfo* out;
            bool* foundBody;
            int* unnamedCounter;
            bool* usedNestedRecord;
            CollectContext* ctx;
        } payload;

        bool foundBody = false;
        bool usedNestedRecord = false;
        int unnamedTemplateParamIndex = 0;
        payload.tu = tu;
        payload.out = &rec;
        payload.foundBody = &foundBody;
        payload.unnamedCounter = &unnamedTemplateParamIndex;
        payload.usedNestedRecord = &usedNestedRecord;
        payload.ctx = &ctx;

        rec.templateKind = RecordTemplateKind::Primary;
        rec.qualifiedName = GetQualifiedName(cursor);
        rec.simpleName = ToStdString(clang_getCursorSpelling(cursor));
        rec.sourcePath = GetCursorSourceFilePath(cursor);
        rec.semanticParentQualifiedName = GetQualifiedName(clang_getCursorSemanticParent(cursor));
        rec.accessPathPublic = CursorAccessPathIsPublicOrTopLevel(cursor);
        rec.isStruct = true;
        if (CursorHasReflectAccessFriend(tu, cursor))
        {
            rec.allowPrivateReflect = true;
            PrintCursorDebugLine("[friend kept]", cursor,
                "owner=[" + rec.qualifiedName + "] reason=class-template-pre-scan-or-macro allowPrivateReflect=yes text=[" +
                ShortenForDebugLog(GetCursorSourceText(tu, cursor)) + "]");
        }
        else
        {
            PrintCursorDebugLine("[friend not found]", cursor,
                "owner=[" + rec.qualifiedName + "] kind=class-template allowPrivateReflect=no text=[" +
                ShortenForDebugLog(GetCursorSourceText(tu, cursor)) + "]");
        }

        clang_visitChildren(
            cursor,
            [](CXCursor child, CXCursor, CXClientData clientData) {
                Payload* payload = static_cast<Payload*>(clientData);
                RecordInfo* out = payload->out;
                bool* foundBody = payload->foundBody;
                int* unnamedCounter = payload->unnamedCounter;
                bool* usedNestedRecord = payload->usedNestedRecord;
                const CXCursorKind kind = clang_getCursorKind(child);

                if (kind == CXCursor_TemplateTypeParameter)
                {
                    std::string name = ToStdString(clang_getCursorSpelling(child));
                    if (name.empty())
                    {
                        std::ostringstream oss;
                        oss << "T" << (*unnamedCounter)++;
                        name = oss.str();
                    }
                    TemplateParamInfo p;
                    p.declText = "typename " + name;
                    p.argText = name;
                    out->templateParams.push_back(p);
                    return CXChildVisit_Continue;
                }

                if (kind == CXCursor_NonTypeTemplateParameter)
                {
                    std::string name = ToStdString(clang_getCursorSpelling(child));
                    if (name.empty())
                    {
                        std::ostringstream oss;
                        oss << "N" << (*unnamedCounter)++;
                        name = oss.str();
                    }
                    std::string typeName = ToStdString(clang_getTypeSpelling(clang_getCursorType(child)));
                    typeName = Trim(typeName);
                    if (typeName.empty()) typeName = "int";
                    TemplateParamInfo p;
                    p.declText = typeName + " " + name;
                    p.argText = name;
                    out->templateParams.push_back(p);
                    return CXChildVisit_Continue;
                }

                if (kind == CXCursor_TemplateTemplateParameter)
                {
                    std::string name = ToStdString(clang_getCursorSpelling(child));
                    if (name.empty())
                    {
                        std::ostringstream oss;
                        oss << "TT" << (*unnamedCounter)++;
                        name = oss.str();
                    }
                    TemplateParamInfo p;
                    p.declText = "typename " + name;
                    p.argText = name;
                    out->templateParams.push_back(p);
                    return CXChildVisit_Continue;
                }

                if (kind == CXCursor_FriendDecl)
                {
                    if (FriendDeclAllowsReflectAccess(payload->tu, child))
                    {
                        out->allowPrivateReflect = true;
                        PrintCursorDebugLine("[friend kept]", child, "owner=[" + out->qualifiedName + "] reason=class-template-friend-decl text=[" + ShortenForDebugLog(GetCursorSourceText(payload->tu, child)) + "]");
                    }
                    return CXChildVisit_Continue;
                }

                if (IsRecordKind(kind))
                {
                    if (IsInsideUnion(child))
                    {
                        PrintCursorDebugLine("[template nested record skipped]", child, "reason=inside-union owner=[" + out->qualifiedName + "]");
                        return CXChildVisit_Continue;
                    }

                    const std::string childQualifiedName = GetQualifiedName(child);
                    const std::string childSimpleName = ToStdString(clang_getCursorSpelling(child));

                    // A ClassTemplate cursor may expose a record cursor for the
                    // templated class body on some libclang versions, but it also
                    // exposes ordinary nested records declared inside the template.
                    //
                    // Example:
                    //   template <int N> struct X {
                    //   public:
                    //       struct Inner { int v; };
                    //   };
                    //
                    // Inner is a type member of X, not a data member of X.  Older
                    // code treated the first nested StructDecl as the template body,
                    // cleared X.fields, and collected Inner::v as if it were X::v.
                    // Only a record whose qualified name is the template record
                    // itself may be used as the template body; all other record
                    // children are nested types and must not populate this template's
                    // fields.
                    const bool isTemplateBodyRecord =
                        (!childQualifiedName.empty() &&
                         childQualifiedName == out->qualifiedName);

                    if (!isTemplateBodyRecord)
                    {
                        PrintCursorDebugLine("[template nested record skipped]", child,
                            "reason=nested-record-not-template-body owner=[" + out->qualifiedName +
                            "] child=[" + childQualifiedName + "] childSimple=[" + childSimpleName + "]");
                        return CXChildVisit_Continue;
                    }

                    out->bases.clear();
                    out->fields.clear();
                    out->isStruct = (kind == CXCursor_StructDecl);
                    CollectRecordBody(payload->tu, child, *out, payload->ctx);
                    *foundBody = true;
                    *usedNestedRecord = true;
                    return CXChildVisit_Continue;
                }

                if (!*usedNestedRecord && kind == CXCursor_CXXBaseSpecifier)
                {
                    BaseInfo b;
                    CXType baseType = clang_getCursorType(child);
                    b.typeName = ToStdString(clang_getTypeSpelling(baseType));
                    b.canonicalTypeName = ToStdString(clang_getTypeSpelling(clang_getCanonicalType(baseType)));
                    CXCursor baseDecl = clang_getTypeDeclaration(baseType);
                    if (!clang_Cursor_isNull(baseDecl)) b.qualifiedTypeName = GetQualifiedName(baseDecl);
                    b.access = NormalizeAccess(clang_getCXXAccessSpecifier(child), out->isStruct);
                    if (!IsSystemCBaseInfo(b) && !b.typeName.empty())
                    {
                        out->bases.push_back(b);
                        if (payload->ctx) ++payload->ctx->basesKept;
                        PrintCursorDebugLine("[base kept]", child, "owner=[" + out->qualifiedName + "] type=[" + b.typeName + "] qtype=[" + b.qualifiedTypeName + "]");
                        *foundBody = true;
                    }
                    else
                    {
                        if (payload->ctx) ++payload->ctx->basesSkippedSystemC;
                        PrintCursorDebugLine("[base skipped]", child, "reason=systemc-or-empty owner=[" + out->qualifiedName + "] type=[" + b.typeName + "] qtype=[" + b.qualifiedTypeName + "]");
                    }
                    return CXChildVisit_Continue;
                }

                if (!*usedNestedRecord && kind == CXCursor_FieldDecl)
                {
                    if (IsInsideUnion(child) || IsUnionFieldCursor(child))
                    {
                        if (payload->ctx) ++payload->ctx->fieldsSkippedSystemC;
                        PrintCursorDebugLine("[field skipped]", child, "reason=union-field-or-inside-union owner=[" + out->qualifiedName + "]");
                        return CXChildVisit_Continue;
                    }
                    FieldInfo f;
                    f.name = ToStdString(clang_getCursorSpelling(child));
                    if (!f.name.empty())
                    {
                        CXType fieldType = clang_getCursorType(child);
                        f.typeName = ToStdString(clang_getTypeSpelling(fieldType));
                        f.canonicalTypeName = ToStdString(clang_getTypeSpelling(clang_getCanonicalType(fieldType)));
                        f.declQualifiedName = GetTypeDeclarationQualifiedName(fieldType);
                        if (ShouldSkipFieldType(f.typeName))
                        {
                            if (payload->ctx) ++payload->ctx->fieldsSkippedSystemC;
                            PrintCursorDebugLine("[field skipped]", child, "reason=systemc-non-whitelist owner=[" + out->qualifiedName + "] field=[" + f.name + "] type=[" + f.typeName + "]");
                            return CXChildVisit_Continue;
                        }
                        if (!FieldTypeAccessPathIsReflectAccessible(payload->tu, child))
                        {
                            if (IsDependentOrTemplateParamFieldType(child))
                            {
                                PrintCursorDebugLine("[field kept]", child, "reason=dependent-field-type-access-check-deferred owner=[" + out->qualifiedName + "] field=[" + f.name + "] type=[" + f.typeName + "] canonical=[" + f.canonicalTypeName + "]");
                            }
                            else
                            {
                                if (payload->ctx) ++payload->ctx->fieldsSkippedEmptyName;
                                PrintCursorDebugLine("[field skipped]", child, "reason=field-type-has-non-public-access-path owner=[" + out->qualifiedName + "] field=[" + f.name + "] type=[" + f.typeName + "] canonical=[" + f.canonicalTypeName + "]");
                                return CXChildVisit_Continue;
                            }
                        }
                        f.access = NormalizeAccess(clang_getCXXAccessSpecifier(child), out->isStruct);
                        f.isBitField = clang_Cursor_isBitField(child) != 0;
                        if (f.isBitField) f.bitWidth = clang_getFieldDeclBitWidth(child);
                        out->fields.push_back(f);
                        if (payload->ctx) ++payload->ctx->fieldsKept;
                        PrintCursorDebugLine("[field kept]", child, "owner=[" + out->qualifiedName + "] field=[" + f.name + "] type=[" + f.typeName + "] access=" + std::to_string(static_cast<int>(f.access)) + " allowPrivateReflect=" + (out->allowPrivateReflect ? "yes" : "no"));
                        *foundBody = true;
                    }
                    else
                    {
                        if (payload->ctx) ++payload->ctx->fieldsSkippedEmptyName;
                        PrintCursorDebugLine("[field skipped]", child, "reason=empty-name owner=[" + out->qualifiedName + "]");
                    }
                    return CXChildVisit_Continue;
                }

                return CXChildVisit_Continue;
            },
            &payload);

        if (!foundBody || rec.qualifiedName.empty())
        {
            ++ctx.classTemplateSkipped;
            std::ostringstream oss;
            oss << "reason=collect-template-failed foundBody=" << (foundBody ? "yes" : "no")
                << " qname=[" << rec.qualifiedName << "]";
            PrintCursorDebugLine("[template skipped]", cursor, oss.str());
            return false;
        }

        PrintCursorDebugLine("[template collected]", cursor, "qname=[" + rec.qualifiedName + "] fields=" + std::to_string(rec.fields.size()) + " bases=" + std::to_string(rec.bases.size()));
        return true;
    }

    bool CollectAnonymousTypedefRecord(CXTranslationUnit tu, CXCursor typedefCursor, CollectContext& ctx, RecordInfo& rec)
    {
        if (clang_getCursorKind(typedefCursor) != CXCursor_TypedefDecl) return false;
        if (IsInsideUnion(typedefCursor)) return false;
        if (!CursorPassesSourceWhitelist(typedefCursor, ctx)) return false;
        if (!CursorAccessIsPublicOrTopLevel(typedefCursor)) return false;

        const std::string typedefName = ToStdString(clang_getCursorSpelling(typedefCursor));
        if (typedefName.empty() || LooksLikeLibclangAnonymousName(typedefName)) return false;

        CXType underlying = clang_getTypedefDeclUnderlyingType(typedefCursor);
        CXCursor recordCursor = clang_getTypeDeclaration(underlying);
        if (clang_Cursor_isNull(recordCursor)) return false;
        CXCursor defCursor = clang_getCursorDefinition(recordCursor);
        if (!clang_Cursor_isNull(defCursor)) recordCursor = defCursor;

        const CXCursorKind recordKind = clang_getCursorKind(recordCursor);
        if (recordKind == CXCursor_UnionDecl) return false;
        if (!IsRecordKind(recordKind)) return false;
        if (!clang_isCursorDefinition(recordCursor)) return false;
        if (IsInsideUnion(recordCursor)) return false;
        if (!CursorPassesSourceWhitelist(recordCursor, ctx)) return false;
        if (IsNonBusinessRecordCursor(recordCursor)) return false;

        const std::string recordSpelling = ToStdString(clang_getCursorSpelling(recordCursor));
        // Named records are handled by their StructDecl/ClassDecl cursor.  The typedef
        // cursor is only used to give a real reflected type name to anonymous C-style
        // declarations such as: typedef struct { int x; } Foo;
        if (!recordSpelling.empty() && !LooksLikeLibclangAnonymousName(recordSpelling)) return false;

        rec.qualifiedName = GetQualifiedName(typedefCursor);
        rec.simpleName = typedefName;
        rec.sourcePath = GetCursorSourceFilePath(typedefCursor);
        if (rec.sourcePath.empty()) rec.sourcePath = GetCursorSourceFilePath(recordCursor);
        rec.isStruct = (recordKind == CXCursor_StructDecl);
        rec.isAnonymousTypedefAlias = true;
        CollectRecordBody(tu, recordCursor, rec, &ctx);
        return !rec.qualifiedName.empty();
    }

    CXChildVisitResult AstVisitor(CXCursor cursor, CXCursor, CXClientData clientData)
    {
        CollectContext* ctx = static_cast<CollectContext*>(clientData);
        const CXCursorKind kind = clang_getCursorKind(cursor);

        if (kind == CXCursor_TypedefDecl)
        {
            RecordInfo rec;
            if (CollectAnonymousTypedefRecord(ctx->tu, cursor, *ctx, rec))
            {
                const std::string visitKey = "typedef-anon:" + rec.qualifiedName;
                if (ctx->visited.insert(visitKey).second)
                {
                    ctx->records.push_back(rec);
                    ++ctx->recordCollected;
                    PrintCursorDebugLine("[typedef-anon-record collected]", cursor, "visitKey=[" + visitKey + "] fields=" + std::to_string(rec.fields.size()) + " bases=" + std::to_string(rec.bases.size()));
                }
                else
                {
                    ++ctx->recordDuplicates;
                    PrintCursorDebugLine("[typedef-anon-record duplicate]", cursor, "visitKey=[" + visitKey + "]");
                }
            }
            return CXChildVisit_Recurse;
        }

        if (kind == CXCursor_ClassTemplate)
        {
            ++ctx->classTemplateCandidates;
            RecordInfo rec;
            if (CollectTemplateRecord(ctx->tu, cursor, *ctx, rec))
            {
                const std::string visitKey = "primary:" + rec.qualifiedName;
                if (ctx->visited.insert(visitKey).second)
                {
                    ctx->records.push_back(rec);
                    ++ctx->classTemplateCollected;
                }
                else
                {
                    ++ctx->recordDuplicates;
                    PrintCursorDebugLine("[template duplicate]", cursor, "visitKey=[" + visitKey + "]");
                }
            }
            return CXChildVisit_Continue;
        }

        if (IsRecordKind(kind))
        {
            ++ctx->recordCandidates;
            const std::string skipReason = GetRecordSkipReason(cursor, *ctx);
            if (!skipReason.empty())
            {
                ++ctx->recordSkipped;
                if (ShouldPrintSkipReason(skipReason))
                {
                    PrintCursorDebugLine("[record skipped]", cursor, "reason=" + skipReason);
                }
                return CXChildVisit_Recurse;
            }

            PrintCursorDebugLine("[record candidate]", cursor, "status=collecting");
            RecordInfo rec;
            rec.qualifiedName = GetQualifiedName(cursor);
            rec.simpleName = ToStdString(clang_getCursorSpelling(cursor));
            rec.sourcePath = GetCursorSourceFilePath(cursor);
            rec.semanticParentQualifiedName = GetQualifiedName(clang_getCursorSemanticParent(cursor));
            rec.accessPathPublic = CursorAccessPathIsPublicOrTopLevel(cursor);
            rec.isStruct = (kind == CXCursor_StructDecl);
            if (IsExplicitTemplateSpecialization(cursor))
            {
                rec.templateKind = RecordTemplateKind::ExplicitSpecialization;
                rec.explicitReflectedType = Trim(ToStdString(clang_getTypeSpelling(clang_getCursorType(cursor))));
            }
            CollectRecordBody(ctx->tu, cursor, rec, ctx);
            std::string visitKey = rec.qualifiedName;
            if (rec.templateKind == RecordTemplateKind::ExplicitSpecialization) visitKey = "spec:" + rec.explicitReflectedType;
            if (!visitKey.empty() && ctx->visited.insert(visitKey).second)
            {
                ctx->records.push_back(rec);
                ++ctx->recordCollected;
                PrintCursorDebugLine("[record collected]", cursor, "visitKey=[" + visitKey + "] fields=" + std::to_string(rec.fields.size()) + " bases=" + std::to_string(rec.bases.size()));
            }
            else
            {
                ++ctx->recordDuplicates;
                PrintCursorDebugLine("[record duplicate-or-empty-key]", cursor, "visitKey=[" + visitKey + "] qname=[" + rec.qualifiedName + "]");
            }
        }
        return CXChildVisit_Recurse;
    }

    // Forward declarations for helpers used by the libclang diagnostic probe.
    // The full definitions are later in this translation unit.
    void AddUniqueArg(std::vector<std::string>& args, const std::string& arg);
    bool MakeDirectoryRecursive(const std::string& dir);

    struct DiagnosticsSummary
    {
        unsigned total = 0;
        unsigned warnings = 0;
        unsigned errors = 0;
        unsigned fatals = 0;

        bool HasErrorOrFatal() const { return errors != 0 || fatals != 0; }
    };

    DiagnosticsSummary PrintDiagnostics(CXTranslationUnit tu)
    {
        DiagnosticsSummary summary;
        const unsigned diagCount = clang_getNumDiagnostics(tu);
        summary.total = diagCount;
        for (unsigned i = 0; i < diagCount; ++i)
        {
            CXDiagnostic diag = clang_getDiagnostic(tu, i);
            const CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(diag);
            if (sev == CXDiagnostic_Warning) ++summary.warnings;
            else if (sev == CXDiagnostic_Error) ++summary.errors;
            else if (sev == CXDiagnostic_Fatal) ++summary.fatals;

            CXString formatted = clang_formatDiagnostic(diag, clang_defaultDiagnosticDisplayOptions());
            std::cerr << ToStdString(formatted) << "\n";
            clang_disposeDiagnostic(diag);
        }
        return summary;
    }

    bool DiagnosticsAreAcceptableOrReport(const Options& opt, const DiagnosticsSummary& diag)
    {
        if (!diag.HasErrorOrFatal()) return true;
        if (opt.allowParseErrors)
        {
            std::cerr << "[diagnostics warning] libclang reported errors=" << diag.errors
                << " fatals=" << diag.fatals
                << ", but --allow-errors was specified. Generated reflection may be incomplete.\n";
            return true;
        }

        std::cerr << "[diagnostics error] libclang reported errors=" << diag.errors
            << " fatals=" << diag.fatals
            << ". Reflection generation is aborted to avoid emitting code from an incomplete AST.\n";
        std::cerr << "[diagnostics hint] Fix the parse errors or pass --allow-errors only for temporary debugging.\n";
        return false;
    }


    const char* CxErrorName(CXErrorCode err)
    {
        switch (err)
        {
        case CXError_Success: return "CXError_Success";
        case CXError_Failure: return "CXError_Failure";
        case CXError_Crashed: return "CXError_Crashed";
        case CXError_InvalidArguments: return "CXError_InvalidArguments";
        case CXError_ASTReadError: return "CXError_ASTReadError";
        default: return "CXError_Unknown";
        }
    }

    void PrintClangArgsForLog(const std::vector<std::string>& args)
    {
        std::size_t includeDirs = 0;
        std::size_t defines = 0;
        std::size_t forcedIncludes = 0;
        std::size_t standards = 0;
        std::size_t other = 0;

        for (std::size_t i = 0; i < args.size(); ++i)
        {
            const std::string& a = args[i];
            if ((a == "-I" || a == "/I") && i + 1 < args.size())
            {
                ++includeDirs;
                ++i;
            }
            else if ((a.compare(0, 2, "-I") == 0 && a.size() > 2) ||
                (a.compare(0, 2, "/I") == 0 && a.size() > 2))
            {
                ++includeDirs;
            }
            else if ((a == "-D" || a == "/D") && i + 1 < args.size())
            {
                ++defines;
                ++i;
            }
            else if ((a.compare(0, 2, "-D") == 0 && a.size() > 2) ||
                (a.compare(0, 2, "/D") == 0 && a.size() > 2))
            {
                ++defines;
            }
            else if ((a == "-include" || a == "/FI") && i + 1 < args.size())
            {
                ++forcedIncludes;
                ++i;
            }
            else if ((a.compare(0, 8, "-include") == 0 && a.size() > 8) ||
                (a.compare(0, 3, "/FI") == 0 && a.size() > 3))
            {
                ++forcedIncludes;
            }
            else if (a.compare(0, 5, "-std=") == 0 || a.compare(0, 5, "/std:") == 0)
            {
                ++standards;
            }
            else
            {
                ++other;
            }
        }

        std::cerr << "[libclang args summary] total=" << args.size()
            << " includeDirs=" << includeDirs
            << " defines=" << defines
            << " forcedIncludes=" << forcedIncludes
            << " standards=" << standards
            << " other=" << other << "\n";

        if (!gDebugAst)
        {
            std::cerr << "[libclang args summary] full argument list omitted; pass --debug-ast for full clang args.\\n";
            return;
        }

        std::cerr << "[libclang args]";
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            std::cerr << " [" << args[i] << "]";
        }
        std::cerr << "\n";
    }

    std::vector<std::string> BuildDiagnosticProbeArgsAndExtractForcedIncludes(
        const Options& opt,
        std::vector<std::string>& forcedIncludes)
    {
        std::vector<std::string> args;
        args.reserve(opt.clangArgs.size() + 8);
        forcedIncludes.clear();

        bool replacedX = false;
        for (std::size_t i = 0; i < opt.clangArgs.size(); ++i)
        {
            const std::string& a = opt.clangArgs[i];

            // Diagnostic-only trick:
            // Remove driver-level -include / /FI from libclang args and re-inject
            // them as textual #include lines in the in-memory probe source. Some
            // PCH-like or driver-level include failures make libclang return a NULL
            // TranslationUnit, which means no diagnostics can be printed. Textual
            // includes usually force the normal parser path and materialize errors.
            if ((a == "-include" || a == "/FI") && i + 1 < opt.clangArgs.size())
            {
                forcedIncludes.push_back(opt.clangArgs[++i]);
                continue;
            }
            if (a.compare(0, 8, "-include") == 0 && a.size() > 8)
            {
                forcedIncludes.push_back(a.substr(8));
                continue;
            }
            if (a.compare(0, 3, "/FI") == 0 && a.size() > 3)
            {
                forcedIncludes.push_back(a.substr(3));
                continue;
            }

            if (a == "-x" && i + 1 < opt.clangArgs.size())
            {
                args.push_back("-x");
                args.push_back("c++");
                ++i;
                replacedX = true;
                continue;
            }
            if (a.compare(0, 2, "-x") == 0 && a.size() > 2)
            {
                args.push_back("-xc++");
                replacedX = true;
                continue;
            }
            args.push_back(a);
        }

        if (!replacedX)
        {
            args.insert(args.begin(), "c++");
            args.insert(args.begin(), "-x");
        }

        AddUniqueArg(args, "-fsyntax-only");
        AddUniqueArg(args, "-ferror-limit=0");
        if (gDebugAst)
        {
            AddUniqueArg(args, "-v");
            AddUniqueArg(args, "-H");
        }

        const std::string inputDir = DirectoryName(opt.inputHeader);
        if (!inputDir.empty()) AddUniqueArg(args, "-I" + inputDir);
        return args;
    }

    void RunLibclangDiagnosticProbe(CXIndex index, const Options& opt)
    {
        std::cerr << "\n========== ReflectGen libclang diagnostic probe ==========" << "\n";
        std::cerr << "[probe note] Original parse failed before a usable TranslationUnit was created.\n";
        std::cerr << "[probe note] Retrying inside ReflectGen with an in-memory .cpp wrapper to force diagnostics.\n";
        std::cerr << "[probe note] Driver-level -include arguments are converted into textual #include lines in this probe.\n";
        std::cerr << "[probe input header] " << opt.inputHeader << "\n";

        std::vector<std::string> forcedIncludes;
        std::vector<std::string> probeArgs = BuildDiagnosticProbeArgsAndExtractForcedIncludes(opt, forcedIncludes);
        PrintClangArgsForLog(probeArgs);

        std::string probeSource;
        probeSource += "// ReflectGen diagnostic probe generated in memory.\n";
        probeSource += "#define REFLECTGEN_DIAGNOSTIC_PROBE 1\n";
        for (std::size_t i = 0; i < forcedIncludes.size(); ++i)
        {
            std::string inc = StripOuterQuotes(forcedIncludes[i]);
            inc = NormalizePathSlashes(inc);
            if (gDebugAst) std::cerr << "[probe textual forced include] " << inc << "\n";
            probeSource += "#include \"" + EscapeString(inc) + "\"\n";
        }
        const std::string includePath = NormalizePathSlashes(MakeAbsoluteLexicalPath(opt.inputHeader));
        probeSource += "#include \"" + EscapeString(includePath) + "\"\n";

        if (gDebugAst)
        {
            std::cerr << "[probe source]\n" << probeSource << "\n";
        }
        else
        {
            std::cerr << "[probe source] omitted; pass --debug-ast to print generated probe source.\n";
        }

        const std::string probeName = "__reflectgen_diagnostic_probe.cpp";
        CXUnsavedFile unsaved;
        unsaved.Filename = probeName.c_str();
        unsaved.Contents = probeSource.c_str();
        unsaved.Length = static_cast<unsigned long>(probeSource.size());

        std::vector<const char*> cargs;
        cargs.reserve(probeArgs.size());
        for (std::size_t i = 0; i < probeArgs.size(); ++i) cargs.push_back(probeArgs[i].c_str());

        CXTranslationUnit probeTu = NULL;
        const CXErrorCode probeErr = clang_parseTranslationUnit2(
            index,
            probeName.c_str(),
            cargs.empty() ? NULL : &cargs[0],
            static_cast<int>(cargs.size()),
            &unsaved,
            1,
            CXTranslationUnit_DetailedPreprocessingRecord,
            &probeTu);

        std::cerr << "[probe libclang result] " << CxErrorName(probeErr)
            << " (" << static_cast<int>(probeErr) << ")\n";

        if (probeTu)
        {
            std::cerr << "[probe diagnostics begin]\n";
            PrintDiagnostics(probeTu);
            std::cerr << "[probe diagnostics end]\n";
            clang_disposeTranslationUnit(probeTu);
        }
        else
        {
            std::cerr << "[probe diagnostics unavailable] libclang still returned NULL TranslationUnit.\n";
            std::cerr << "[probe hint] In this case the failure is usually before diagnostics are materialized: bad -include/PCH, incompatible AST/PCH/module input, or fatal argument/configuration mismatch.\n";
        }
        std::cerr << "========== End ReflectGen libclang diagnostic probe ==========" << "\n\n";
    }

    std::string ShellQuote(const std::string& raw)
    {
        if (raw.empty()) return "\"\"";
        std::string out;
        out.reserve(raw.size() + 2);
        out += '"';
        for (std::size_t i = 0; i < raw.size(); ++i)
        {
            const char c = raw[i];
            if (c == '"') out += "\\\"";
            else out += c;
        }
        out += '"';
        return out;
    }

    std::string GetEnvStringForClangxx(const char* name)
    {
        const char* v = std::getenv(name);
        return v ? std::string(v) : std::string();
    }

    std::string JoinSimplePathForClangxx(std::string a, const std::string& b)
    {
        a = NormalizePathSlashes(StripOuterQuotes(ExpandEnvironmentVariablesInPath(a)));
        if (a.empty()) return NormalizePathSlashes(b);
        if (a[a.size() - 1] != '/' && a[a.size() - 1] != '\\') a += '/';
        return NormalizePathSlashes(a + b);
    }

    std::string ResolveClangxxPath(const Options& opt)
    {
        std::string p = StripOuterQuotes(ExpandEnvironmentVariablesInPath(opt.clangxxPath));
        if (!p.empty()) return NormalizePathSlashes(p);

        p = GetEnvStringForClangxx("CLANGXX");
        if (!p.empty()) return NormalizePathSlashes(StripOuterQuotes(ExpandEnvironmentVariablesInPath(p)));

        p = GetEnvStringForClangxx("CLANGXX_PATH");
        if (!p.empty()) return NormalizePathSlashes(StripOuterQuotes(ExpandEnvironmentVariablesInPath(p)));

        p = GetEnvStringForClangxx("LLVM_ROOT");
        if (!p.empty())
        {
#ifdef _WIN32
            return JoinSimplePathForClangxx(p, "bin/clang++.exe");
#else
            return JoinSimplePathForClangxx(p, "bin/clang++");
#endif
        }

#ifdef _WIN32
        return "clang++.exe";
#else
        return "clang++";
#endif
    }

    std::vector<std::string> BuildClangxxDiagnosticArgs(const Options& opt)
    {
        std::vector<std::string> args;
        args.reserve(opt.clangArgs.size() + 12);

        bool replacedX = false;
        for (std::size_t i = 0; i < opt.clangArgs.size(); ++i)
        {
            const std::string& a = opt.clangArgs[i];
            if (a == "-x" && i + 1 < opt.clangArgs.size())
            {
                args.push_back("-x");
                args.push_back("c++");
                ++i;
                replacedX = true;
                continue;
            }
            if (a.compare(0, 2, "-x") == 0 && a.size() > 2)
            {
                args.push_back("-xc++");
                replacedX = true;
                continue;
            }
            args.push_back(a);
        }

        if (!replacedX)
        {
            args.insert(args.begin(), "c++");
            args.insert(args.begin(), "-x");
        }

        AddUniqueArg(args, "-fsyntax-only");
        AddUniqueArg(args, "-ferror-limit=0");
        if (gDebugAst)
        {
            AddUniqueArg(args, "-v");
            AddUniqueArg(args, "-H");
        }

        const std::string inputDir = DirectoryName(opt.inputHeader);
        if (!inputDir.empty()) AddUniqueArg(args, "-I" + inputDir);
        return args;
    }

    bool FileExistsForDiagnostics(const std::string& path)
    {
        if (path.empty()) return false;
        struct stat st;
        return stat(path.c_str(), &st) == 0;
    }

    bool LooksLikeExplicitPath(const std::string& path)
    {
        return path.find('/') != std::string::npos ||
            path.find('\\') != std::string::npos ||
            (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':');
    }

    std::string AbsoluteDiagnosticPathOrEmpty(std::string path)
    {
        path = NormalizePathSlashes(StripOuterQuotes(ExpandEnvironmentVariablesInPath(path)));
        if (path.empty()) return std::string();
        return MakeAbsoluteLexicalPath(path);
    }

    std::string CaptureProcessOutput(const std::string& cmd, int& rc)
    {
        rc = -1;
        std::string out;
#ifdef _WIN32
        FILE* pipe = _popen(cmd.c_str(), "r");
#else
        FILE* pipe = popen(cmd.c_str(), "r");
#endif
        if (!pipe)
        {
            out += "[ReflectGen capture error] failed to start command through popen/_popen.\n";
            out += "[ReflectGen capture command] " + cmd + "\n";
            return out;
        }

        char buffer[4096];
        while (true)
        {
            const std::size_t n = std::fread(buffer, 1, sizeof(buffer), pipe);
            if (n > 0) out.append(buffer, n);
            if (n < sizeof(buffer))
            {
                if (std::feof(pipe)) break;
                if (std::ferror(pipe))
                {
                    out += "\n[ReflectGen capture warning] fread reported an error while reading child process output.\n";
                    break;
                }
            }
        }

#ifdef _WIN32
        rc = _pclose(pipe);
#else
        rc = pclose(pipe);
#endif
        return out;
    }

    void PrintPathCheck(const std::string& label, const std::string& path)
    {
        std::cerr << label << " " << path << "\n";
        if (path.empty())
        {
            std::cerr << label << " exists: no, empty path\n";
            return;
        }
        if (LooksLikeExplicitPath(path))
        {
            const std::string abs = MakeAbsoluteLexicalPath(path);
            std::cerr << label << " absolute: " << abs << "\n";
            std::cerr << label << " exists: " << (FileExistsForDiagnostics(abs) ? "yes" : "NO") << "\n";
        }
        else
        {
            std::cerr << label << " has no slash; it will be resolved by PATH/cmd.\n";
        }
    }

    std::string LastSystemErrorForLog()
    {
        std::ostringstream oss;
        const int e = errno;
        bool wrote = false;
        if (e != 0)
        {
            oss << "errno=" << e << " (" << std::strerror(e) << ")";
            wrote = true;
        }
#ifdef _WIN32
        const DWORD gle = GetLastError();
        if (gle != 0)
        {
            if (wrote) oss << "; ";
            LPSTR msg = NULL;
            const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
            const DWORD n = FormatMessageA(flags, NULL, gle, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&msg), 0, NULL);
            oss << "GetLastError=" << static_cast<unsigned long>(gle);
            if (n != 0 && msg != NULL)
            {
                std::string s(msg, msg + n);
                while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.erase(s.size() - 1);
                if (!s.empty()) oss << " (" << s << ")";
            }
            if (msg != NULL) LocalFree(msg);
            wrote = true;
        }
#endif
        if (!wrote) return "no errno/GetLastError details available";
        return oss.str();
    }

    bool DirectoryExistsForDiagnostics(const std::string& path)
    {
        if (path.empty()) return false;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) return false;
#ifdef _WIN32
        return (st.st_mode & _S_IFDIR) != 0;
#else
        return (st.st_mode & S_IFDIR) != 0;
#endif
    }

    void PrintPathFailureContext(const std::string& label, const std::string& path)
    {
        std::cerr << label << " path: " << path << "\n";
        std::cerr << label << " cwd: " << CurrentWorkingDirectory() << "\n";
        if (!path.empty())
        {
            const std::string abs = MakeAbsoluteLexicalPath(path);
            const std::string parent = DirectoryName(abs);
            std::cerr << label << " absolute: " << abs << "\n";
            std::cerr << label << " parent: " << parent << "\n";
            std::cerr << label << " parent exists: " << (DirectoryExistsForDiagnostics(parent) ? "yes" : "NO") << "\n";
            std::cerr << label << " file exists: " << (FileExistsForDiagnostics(abs) ? "yes" : "NO") << "\n";
        }
        std::cerr << label << " system error: " << LastSystemErrorForLog() << "\n";
    }

    void PrintDirectoryCreateFailure(const std::string& label, const std::string& dir)
    {
        PrintPathFailureContext(label + " directory create failed", dir);
    }

    void PrintFileOpenFailure(const std::string& label, const std::string& path)
    {
        PrintPathFailureContext(label + " file open/write failed", path);
    }

    bool FlushAndCheckFile(std::ofstream& os, const std::string& path, const std::string& label)
    {
        errno = 0;
        os.flush();
        if (!os)
        {
            PrintFileOpenFailure(label, path);
            return false;
        }
        return true;
    }


    void RunClangxxDiagnosticProbe(const Options& opt)
    {
        const std::string clangxx = NormalizePathSlashes(StripOuterQuotes(ExpandEnvironmentVariablesInPath(ResolveClangxxPath(opt))));
        const std::vector<std::string> args = BuildClangxxDiagnosticArgs(opt);
        const std::string input = NormalizePathSlashes(MakeAbsoluteLexicalPath(opt.inputHeader));

        std::string logPath = AbsoluteDiagnosticPathOrEmpty(opt.logFile);
        if (logPath.empty()) logPath = JoinPath(CurrentWorkingDirectory(), "reflectgen.log");
        const std::string logDir = DirectoryName(logPath);
        if (!logDir.empty() && !MakeDirectoryRecursive(logDir))
        {
            PrintDirectoryCreateFailure("[diagnostic log path]", logDir);
        }

        std::string tempBase = logPath;
        if (tempBase.empty()) tempBase = JoinPath(CurrentWorkingDirectory(), "reflectgen");
        const std::string rspPath = tempBase + ".clangxx.rsp";
        const std::string outPath = tempBase + ".clangxx.popen.txt";

        std::cerr << "\n========== ReflectGen clang++ diagnostic fallback ==========" << "\n";
        std::cerr << "[diagnostic cwd] " << CurrentWorkingDirectory() << "\n";
        std::cerr << "[diagnostic log path] " << logPath << "\n";
        std::cerr << "[diagnostic response file] " << rspPath << "\n";
        std::cerr << "[diagnostic captured mirror file] " << outPath << "\n";
        PrintPathCheck("[clang++ path]", clangxx);
        PrintPathCheck("[clang++ input]", input);
        std::cerr << "[clang++ note] This is diagnostic-only. Reflection generation still uses libclang.\n";
        if (gDebugAst)
        {
            std::cerr << "[clang++ args]";
            for (std::size_t i = 0; i < args.size(); ++i) std::cerr << " [" << args[i] << "]";
            std::cerr << "\n";
        }
        else
        {
            std::cerr << "[clang++ args summary] total=" << args.size()
                << "; full argument list omitted; pass --debug-ast for details.\n";
        }

        const std::string rspDir = DirectoryName(rspPath);
        if (!rspDir.empty() && !MakeDirectoryRecursive(rspDir))
        {
            PrintDirectoryCreateFailure("[clang++ response file]", rspDir);
        }

        bool rspOk = false;
        {
            errno = 0;
            std::ofstream rsp(rspPath.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
            if (rsp)
            {
                for (std::size_t i = 0; i < args.size(); ++i)
                {
                    rsp << ShellQuote(args[i]) << "\n";
                }
                rsp << ShellQuote(input) << "\n";
                rspOk = FlushAndCheckFile(rsp, rspPath, "[clang++ response file write error]");
            }
            else
            {
                PrintFileOpenFailure("[clang++ response file error]", rspPath);
            }
        }

        if (rspOk)
        {
            std::ifstream rspIn(rspPath.c_str(), std::ios::in | std::ios::binary);
            if (rspIn)
            {
                std::ostringstream ss;
                ss << rspIn.rdbuf();
                std::string rspText = ss.str();
                std::cerr << "[clang++ response file exists] yes\n";
                if (gDebugAst)
                {
                    std::cerr << "========== clang++ response file begin ==========" << "\n";
                    std::cerr << rspText;
                    if (!rspText.empty() && rspText[rspText.size() - 1] != '\n') std::cerr << "\n";
                    std::cerr << "========== clang++ response file end ==========" << "\n";
                }
                else
                {
                    std::cerr << "[clang++ response file] content omitted; pass --debug-ast to print it.\n";
                }
            }
            else
            {
                PrintFileOpenFailure("[clang++ response file reopen error]", rspPath);
            }
        }

        std::string cmd;
        if (rspOk)
        {
            cmd = ShellQuote(clangxx) + " @" + ShellQuote(rspPath);
        }
        else
        {
            cmd = ShellQuote(clangxx);
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                cmd += " ";
                cmd += ShellQuote(args[i]);
            }
            cmd += " ";
            cmd += ShellQuote(input);
        }

        cmd += " 2>&1";

        std::cerr << "[clang++ command] " << cmd << "\n";
        std::cout.flush();
        std::cerr.flush();

        int rc = -1;
        const std::string captured = CaptureProcessOutput(cmd, rc);
        std::cerr << "[clang++ exit code] " << rc << "\n";

        {
            const std::string outDir = DirectoryName(outPath);
            if (!outDir.empty() && !MakeDirectoryRecursive(outDir))
            {
                PrintDirectoryCreateFailure("[clang++ mirror output]", outDir);
            }
            errno = 0;
            std::ofstream mirror(outPath.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
            if (mirror)
            {
                mirror << captured;
                FlushAndCheckFile(mirror, outPath, "[clang++ mirror output write error]");
            }
            else
            {
                PrintFileOpenFailure("[clang++ mirror output warning]", outPath);
            }
        }

        std::cerr << "\n========== clang++ captured diagnostics begin ==========" << "\n";
        if (captured.empty())
        {
            std::cerr << "[clang++ captured diagnostics empty]\n";
            std::cerr << "[hint] The process produced no stdout/stderr. If the clang path exists, this may be a DLL-load failure that Windows does not print to the pipe. Try running the printed command manually, or copy all LLVM *.dll files beside clang.exe.\n";
        }
        else
        {
            std::cerr << captured;
            if (captured[captured.size() - 1] != '\n') std::cerr << "\n";
        }
        std::cerr << "========== clang++ captured diagnostics end ==========" << "\n";

        std::cerr << "========== End ReflectGen clang++ diagnostic fallback ==========" << "\n\n";
    }

    std::string EnsureGlobalQualifiedType(std::string typeName)
    {
        typeName = Trim(typeName);
        if (typeName.empty()) return typeName;
        if (typeName.compare(0, 2, "::") == 0) return typeName;
        return "::" + typeName;
    }

    std::string StripLeadingGlobalQualifier(std::string typeName)
    {
        typeName = Trim(typeName);
        if (typeName.compare(0, 2, "::") == 0) return typeName.substr(2);
        return typeName;
    }

    std::string NormalizeBoolStorageTypedefSpelling(std::string typeName)
    {
        typeName = Trim(typeName);
        bool changed = true;
        while (changed)
        {
            changed = false;
            changed = ConsumePrefix(typeName, "const ") || changed;
            changed = ConsumePrefix(typeName, "volatile ") || changed;
            changed = ConsumePrefix(typeName, "::") || changed;
            typeName = Trim(typeName);
        }
        return typeName;
    }

    bool IsBoolStorageTypedefSpelling(const Options& opt, const std::string& typeName)
    {
        const std::string normalized = NormalizeBoolStorageTypedefSpelling(typeName);
        for (std::size_t i = 0; i < opt.boolStorageTypedefs.size(); ++i)
        {
            if (normalized == NormalizeBoolStorageTypedefSpelling(opt.boolStorageTypedefs[i])) return true;
        }
        return false;
    }

    std::string BuildTemplateArgListForRecord(const RecordInfo& rec)
    {
        if (rec.templateKind == RecordTemplateKind::None) return std::string();
        std::ostringstream oss;
        oss << "<";
        for (std::size_t i = 0; i < rec.templateParams.size(); ++i)
        {
            if (i) oss << ", ";
            oss << rec.templateParams[i].argText;
        }
        oss << ">";
        return oss.str();
    }

    std::string BuildReflectedTypeNameUnqualifiedRoot(const RecordInfo& rec)
    {
        if (rec.templateKind == RecordTemplateKind::ExplicitSpecialization && !rec.explicitReflectedType.empty())
        {
            return rec.explicitReflectedType;
        }
        return rec.qualifiedName + BuildTemplateArgListForRecord(rec);
    }

    std::string BuildReflectedTypeNameGlobal(const RecordInfo& rec)
    {
        return EnsureGlobalQualifiedType(BuildReflectedTypeNameUnqualifiedRoot(rec));
    }

    std::string BuildElaboratedReflectedTypeNameGlobal(const RecordInfo& rec)
    {
        const std::string globalName = BuildReflectedTypeNameGlobal(rec);
        const std::string emittedName = StripLeadingGlobalQualifier(globalName);

        // C-style anonymous typedef records have no tag name.  For example:
        //   typedef struct { int x; } Foo;
        // It must be emitted as Foo, not as an elaborated specifier.
        if (rec.isAnonymousTypedefAlias) return emittedName;

        // Do NOT add class-key elaboration to templates.  Keep template
        // primary/specialization spellings exactly as generated, but omit the leading
        // global qualifier for MSVC/IntelliSense compatibility in generated headers.
        if (rec.templateKind != RecordTemplateKind::None) return emittedName;

        // Disambiguate only non-template, real tag types from same-named data members.
        // MSVC/IntelliSense may reject spellings such as "struct ::ns::Type", so the
        // emitted elaborated type uses "struct ns::Type" / "class ns::Type".
        return std::string(rec.isStruct ? "struct " : "class ") + emittedName;
    }

    const RecordInfo* FindRecord(const std::map<std::string, const RecordInfo*>& byName, const std::string& typeName)
    {
        std::map<std::string, const RecordInfo*>::const_iterator it = byName.find(CanonicalRecordLookupKey(typeName));
        return it == byName.end() ? NULL : it->second;
    }

    const RecordInfo* FindRecordForBase(const std::map<std::string, const RecordInfo*>& byName, const BaseInfo& base)
    {
        if (!base.qualifiedTypeName.empty())
        {
            const RecordInfo* rec = FindRecord(byName, base.qualifiedTypeName);
            if (rec) return rec;
        }
        return FindRecord(byName, base.typeName);
    }

    std::string StripClassKeyQualifiersFromTypeSpelling(std::string name)
    {
        name = Trim(name);
        bool changed = true;
        while (changed)
        {
            changed = false;
            changed = ConsumePrefix(name, "::") || changed;
            changed = ConsumePrefix(name, "class ") || changed;
            changed = ConsumePrefix(name, "struct ") || changed;
            changed = ConsumePrefix(name, "union ") || changed;
            changed = ConsumePrefix(name, "const ") || changed;
            changed = ConsumePrefix(name, "volatile ") || changed;
            name = Trim(name);
        }

        // Some libclang/MSVC configurations include elaborated class keys inside
        // template arguments, e.g. Base<class ns::Arg>.  Those spellings are noisy
        // and can break generated code under MSVC.  Remove them as token prefixes.
        const char* keys[] = { "class ", "struct ", "union " };
        for (std::size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k)
        {
            const std::string key(keys[k]);
            std::size_t pos = 0;
            while ((pos = name.find(key, pos)) != std::string::npos)
            {
                const bool atTokenBoundary = (pos == 0) ||
                    !(std::isalnum(static_cast<unsigned char>(name[pos - 1])) || name[pos - 1] == '_');
                if (atTokenBoundary) name.erase(pos, key.size());
                else pos += key.size();
            }
        }
        return Trim(name);
    }

    std::string BuildActualBaseTypeNameGlobal(const BaseInfo& base)
    {
        std::string actual = StripClassKeyQualifiersFromTypeSpelling(base.typeName);
        std::string qname = StripClassKeyQualifiersFromTypeSpelling(base.qualifiedTypeName);

        // Critical case:
        //   struct Derived : Base<int> { ... };
        // clang_getTypeDeclaration(baseType) resolves to the primary template Base,
        // so qualifiedTypeName is "ns::Base" and loses the concrete "<int>".
        // The static_cast must use the actual base specialization, i.e.
        //   static_cast<const ::ns::Base<int>*>(obj)
        // not
        //   static_cast<const ::ns::Base<T>*>(obj)
        // and not just ::ns::Base.
        const std::size_t lt = actual.find('<');
        if (!qname.empty())
        {
            if (lt != std::string::npos)
            {
                return EnsureGlobalQualifiedType(qname + actual.substr(lt));
            }
            return EnsureGlobalQualifiedType(qname);
        }

        return EnsureGlobalQualifiedType(actual);
    }

    std::string BuildBaseTypeForEmission(const BaseInfo& base, const RecordInfo* baseRec)
    {
        // For a template base instance, baseRec describes the primary template and
        // BuildElaboratedReflectedTypeNameGlobal(*baseRec) would produce something
        // like Base<T>, which is invalid in a non-template derived specialization
        // and wrong for static_cast.  Always prefer the actual base-specifier type
        // spelling when the base is a template instance.
        const std::string actualBaseType = StripLeadingGlobalQualifier(BuildActualBaseTypeNameGlobal(base));
        if (actualBaseType.find('<') != std::string::npos) return actualBaseType;

        if (baseRec) return BuildElaboratedReflectedTypeNameGlobal(*baseRec);
        return actualBaseType;
    }

    void CollectMembersRecursive(
        const Options& opt,
        const RecordInfo& rec,
        const std::map<std::string, const RecordInfo*>& byName,
        const std::string& constObjExpr,
        const std::string& mutObjExpr,
        std::set<std::string>& stack,
        std::vector<EmittedMember>& out)
    {
        if (!stack.insert(rec.qualifiedName).second) return;

        std::set<std::string> directNames;
        for (std::size_t i = 0; i < rec.fields.size(); ++i)
        {
            const FieldInfo& f = rec.fields[i];
            if (!RecordAllowsAccess(rec, f.access))
            {
                if (gDebugAst)
                {
                    std::cout << "[field emit skipped] owner=" << rec.qualifiedName
                        << " field=" << f.name
                        << " type=" << f.typeName
                        << " reason=field-access-not-allowed allowPrivateReflect="
                        << (rec.allowPrivateReflect ? "yes" : "no") << "\n";
                }
                continue;
            }
            EmittedMember m;
            m.displayName = f.name;
            m.typeName = f.typeName;
            m.isBitField = f.isBitField;
            m.bitWidth = f.bitWidth;
            m.exprIsPointerAlready = false;
            m.asBoolStorage = !f.isBitField && IsBoolStorageTypedefSpelling(opt, f.typeName);
            m.constExpr = constObjExpr + "->" + f.name;
            m.mutExpr = mutObjExpr + "->" + f.name;
            out.push_back(m);
            directNames.insert(m.displayName);
            if (gDebugAst)
            {
                std::cout << "[field emit kept] owner=" << rec.qualifiedName
                    << " field=" << f.name
                    << " type=" << f.typeName
                    << " canonical=" << f.canonicalTypeName
                    << " bitfield=" << (f.isBitField ? "yes" : "no")
                    << " access=" << static_cast<int>(f.access)
                    << " allowPrivateReflect=" << (rec.allowPrivateReflect ? "yes" : "no")
                    << "\n";
            }
        }

        // Do not recursively emit base-class members.
        //
        // Earlier versions flattened inherited fields into the derived type
        // visitor.  That made generated paths shorter, but it also introduced
        // fragile static_cast code for template base instances, mixed direct
        // fields with inherited fields, and could accidentally expose private or
        // otherwise inaccessible base internals.
        //
        // Current rule: a reflected object only emits the fields declared
        // directly in that record.  Base classes may still be reflected as
        // independent records if they are explicitly selected / reached by other
        // rules, but a derived visitor does not recursively enumerate its base
        // members.
        (void)byName;

        stack.erase(rec.qualifiedName);
    }

    std::vector<EmittedMember> BuildFlattenedMembers(const Options& opt, const RecordInfo& rec, const std::map<std::string, const RecordInfo*>& byName)
    {
        std::set<std::string> stack;
        std::vector<EmittedMember> out;
        CollectMembersRecursive(opt, rec, byName, "obj", "obj", stack, out);
        return out;
    }

    std::string BuildTemplatePrefix(const RecordInfo& rec)
    {
        if (rec.templateKind == RecordTemplateKind::None) return std::string();
        std::ostringstream oss;
        oss << "template <";
        for (std::size_t i = 0; i < rec.templateParams.size(); ++i)
        {
            if (i) oss << ", ";
            oss << rec.templateParams[i].declText;
        }
        oss << ">\n";
        return oss.str();
    }

    std::string BuildTemplateArgList(const RecordInfo& rec)
    {
        if (rec.templateKind == RecordTemplateKind::None) return std::string();
        std::ostringstream oss;
        oss << "<";
        for (std::size_t i = 0; i < rec.templateParams.size(); ++i)
        {
            if (i) oss << ", ";
            oss << rec.templateParams[i].argText;
        }
        oss << ">";
        return oss.str();
    }

    std::string BuildClassSpecializationPrefix(const RecordInfo& rec)
    {
        switch (rec.templateKind)
        {
        case RecordTemplateKind::None:
            return "template <>\n";
        case RecordTemplateKind::Primary:
            return BuildTemplatePrefix(rec);
        case RecordTemplateKind::ExplicitSpecialization:
            return "template <>\n";
        }
        return std::string();
    }

    std::string BuildVisitObjectTemplatePrefix(const RecordInfo& rec)
    {
        std::ostringstream oss;
        oss << "template <";
        bool first = true;
        if (rec.templateKind == RecordTemplateKind::Primary)
        {
            for (std::size_t i = 0; i < rec.templateParams.size(); ++i)
            {
                if (!first) oss << ", ";
                oss << rec.templateParams[i].declText;
                first = false;
            }
        }
        if (!first) oss << ", ";
        oss << "typename PtrVisitor, typename ValueVisitor>\n";
        return oss.str();
    }

    std::string BuildBitGetterName(const std::string& fieldDisplayName)
    {
        std::string out = "get_";
        for (std::size_t i = 0; i < fieldDisplayName.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(fieldDisplayName[i]);
            out += std::isalnum(c) ? static_cast<char>(c) : '_';
        }
        return out;
    }

    std::string BuildReflectedType(const RecordInfo& rec)
    {
        return BuildElaboratedReflectedTypeNameGlobal(rec);
    }

    std::string StripExtension(const std::string& path)
    {
        const std::string base = GetBaseName(path);
        const std::size_t dot = base.find_last_of('.');
        return dot == std::string::npos ? base : base.substr(0, dot);
    }

    std::string SanitizeIdentifier(const std::string& text)
    {
        std::string out;
        out.reserve(text.size() + 8);
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            out += std::isalnum(c) ? static_cast<char>(c) : '_';
        }
        if (out.empty()) out = "generated";
        if (std::isdigit(static_cast<unsigned char>(out[0]))) out.insert(out.begin(), '_');
        return out;
    }

    std::string GetOutputStem(const Options& opt)
    {
        return SanitizeIdentifier(StripExtension(opt.outputHeader));
    }

    std::string NormalizePathSlashes(std::string path)
    {
        for (std::size_t i = 0; i < path.size(); ++i)
        {
            if (path[i] == '\\') path[i] = '/';
        }
        while (path.size() > 1 && path[path.size() - 1] == '/') path.erase(path.size() - 1);
        return path;
    }

    bool IsAbsolutePath(const std::string& path)
    {
        if (path.empty()) return false;
        if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') return true;
        return path[0] == '/' || path[0] == '\\';
    }

    std::string JoinPath(const std::string& a, const std::string& b)
    {
        if (a.empty()) return NormalizePathSlashes(b);
        if (b.empty()) return NormalizePathSlashes(a);
        if (IsAbsolutePath(b)) return NormalizePathSlashes(b);
        std::string out = NormalizePathSlashes(a);
        if (!out.empty() && out[out.size() - 1] != '/') out += '/';
        out += NormalizePathSlashes(b);
        return out;
    }

    std::string DirectoryName(const std::string& path)
    {
        const std::string n = NormalizePathSlashes(path);
        const std::size_t p = n.find_last_of('/');
        if (p == std::string::npos) return std::string();
        if (p == 0) return "/";
        return n.substr(0, p);
    }

    std::string FileNameOnly(const std::string& path)
    {
        const std::string n = NormalizePathSlashes(path);
        const std::size_t p = n.find_last_of('/');
        return p == std::string::npos ? n : n.substr(p + 1);
    }

    bool HasHeaderExtension(const std::string& path)
    {
        const std::string n = NormalizePathSlashes(path);
        const std::size_t p = n.find_last_of('.');
        if (p == std::string::npos) return false;
        std::string ext = n.substr(p);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx";
    }

    bool HasHOrHppExtension(const std::string& path)
    {
        const std::string n = NormalizePathSlashes(path);
        const std::size_t p = n.find_last_of('.');
        if (p == std::string::npos) return false;
        std::string ext = n.substr(p);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".h" || ext == ".hpp";
    }


    std::string ToLowerAscii(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    bool EndsWithString(const std::string& s, const std::string& suffix)
    {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    bool ContainsPathComponent(const std::string& normalizedPath, const std::string& componentLower)
    {
        const std::string p = "/" + ToLowerAscii(NormalizePathSlashes(normalizedPath)) + "/";
        return p.find("/" + componentLower + "/") != std::string::npos;
    }

    bool IsGeneratedReflectHeaderName(const std::string& lowerFileName)
    {
        return EndsWithString(lowerFileName, "_reflect_auto.h") ||
            EndsWithString(lowerFileName, "_reflect_auto.hpp") ||
            EndsWithString(lowerFileName, "_reflect_auto.hh") ||
            EndsWithString(lowerFileName, "_reflect_auto.hxx");
    }

    bool IsReflectionSystemHeaderCandidate(const std::string& path)
    {
        const std::string normalized = NormalizePathSlashes(path);
        const std::string lowerPath = ToLowerAscii(normalized);
        const std::string file = ToLowerAscii(FileNameOnly(normalized));

        // Never re-reflect generated outputs. This is the common failure mode when the
        // output directory or a previous *_reflect_auto.h lives inside a scanned business dir.
        if (ContainsPathComponent(lowerPath, "generated_reflect")) return true;
        if (IsGeneratedReflectHeaderName(file)) return true;
        if (file == "project_reflect_auto.h" || file == "project_reflect_auto.hpp") return true;
        if (file == "__reflectgen_batch_all_headers__.h" || file == "__reflectgen_batch_all_headers__.hpp") return true;

        // Project umbrella/environment/PCH-like headers are dependencies, not business types.
        // They may be used through -include or included by real business headers,
        // but should not be parsed as standalone reflection targets.
        if (file == "aq.h") return true;
        if (file == "aqcm.hpp" || file == "aqcm.h") return true;
        if (file == "aqdebug.hpp" || file == "aqdebug.h") return true;
        if (file == "gcdefines.h" || file == "gcdefines.hpp") return true;

        // Reflection/wave infrastructure headers are dependencies, not business types.
        if (file == "reflect_runtime.h" || file == "reflect_runtime.hpp") return true;
        if (file == "reflect_runtime_explicit.h" || file == "reflect_runtime_explicit.hpp") return true;
        if (file == "reflect_macro.h" || file == "reflect_macro.hpp") return true;
        if (file == "reflect_macro_explicit.h" || file == "reflect_macro_explicit.hpp") return true;
        if (file == "reflect_force_access.h" || file == "reflect_force_access.hpp") return true;
        if (file == "reflect_force_new_hook.h" || file == "reflect_force_new_hook.hpp") return true;
        if (file == "wave_runtime.h" || file == "wave_runtime.hpp") return true;
        if (file == "wave_path_wvz3_recorder.h" || file == "wave_path_wvz3_recorder.hpp") return true;
        if (file == "wvz3_writer_typed.h" || file == "wvz3_writer_typed.hpp") return true;
        if (file == "wvz3_writer.h" || file == "wvz3_writer.hpp") return true;
        if (file == "wvz3_reader.h" || file == "wvz3_reader.hpp") return true;

        return false;
    }

    std::string ReplaceHeaderExtensionWithReflectAuto(const std::string& rel)
    {
        const std::string n = NormalizePathSlashes(rel);
        const std::size_t p = n.find_last_of('.');
        if (p == std::string::npos) return n + "_reflect_auto.h";
        std::string ext = n.substr(p);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".h" && ext != ".hpp" && ext != ".hh" && ext != ".hxx") ext = ".h";
        return n.substr(0, p) + "_reflect_auto" + ext;
    }

    bool StartsWithPathPrefix(const std::string& path, const std::string& prefix)
    {
        const std::string p = NormalizePathSlashes(path);
        std::string pre = NormalizePathSlashes(prefix);
        if (pre.empty()) return false;
        if (p == pre) return true;
        if (pre[pre.size() - 1] != '/') pre += '/';
        return p.compare(0, pre.size(), pre) == 0;
    }

    std::string MakeRelativeToRoot(const std::string& path, const std::string& root)
    {
        const std::string p = NormalizePathSlashes(path);
        std::string r = NormalizePathSlashes(root);
        if (!r.empty() && r[r.size() - 1] != '/') r += '/';
        if (!r.empty() && p.compare(0, r.size(), r) == 0) return p.substr(r.size());
        return FileNameOnly(p);
    }

    std::string StripInlineCommentAndTrim(const std::string& line)
    {
        std::string s = line;
        const std::size_t hash = s.find('#');
        const std::size_t slashSlash = s.find("//");
        std::size_t cut = std::string::npos;
        if (hash != std::string::npos) cut = hash;
        if (slashSlash != std::string::npos) cut = (cut == std::string::npos) ? slashSlash : (std::min)(cut, slashSlash);
        if (cut != std::string::npos) s.erase(cut);
        return Trim(s);
    }

    std::string GetEnvVarForPathExpansion(const std::string& name)
    {
        if (name.empty()) return std::string();
        const char* v = std::getenv(name.c_str());
        return v ? std::string(v) : std::string();
    }

    std::string ExpandEnvironmentVariablesInPath(std::string value)
    {
        value = Trim(value);
        if (value.empty()) return value;

        // Windows/cmd style: %P4ROOT%\dir, %AQROOT%\arch\...
        std::size_t pos = 0;
        while ((pos = value.find('%', pos)) != std::string::npos)
        {
            const std::size_t end = value.find('%', pos + 1);
            if (end == std::string::npos) break;
            const std::string name = value.substr(pos + 1, end - pos - 1);
            const std::string env = GetEnvVarForPathExpansion(name);
            if (!env.empty())
            {
                value.replace(pos, end - pos + 1, env);
                pos += env.size();
            }
            else
            {
                pos = end + 1;
            }
        }

        // MSBuild-like style also allowed in reflect_dirs.txt: $(P4ROOT)\dir
        pos = 0;
        while ((pos = value.find("$(", pos)) != std::string::npos)
        {
            const std::size_t end = value.find(')', pos + 2);
            if (end == std::string::npos) break;
            const std::string name = value.substr(pos + 2, end - pos - 2);
            const std::string env = GetEnvVarForPathExpansion(name);
            if (!env.empty())
            {
                value.replace(pos, end - pos + 1, env);
                pos += env.size();
            }
            else
            {
                pos = end + 1;
            }
        }

        // POSIX/CMake-like style: ${P4ROOT}/dir
        pos = 0;
        while ((pos = value.find("${", pos)) != std::string::npos)
        {
            const std::size_t end = value.find('}', pos + 2);
            if (end == std::string::npos) break;
            const std::string name = value.substr(pos + 2, end - pos - 2);
            const std::string env = GetEnvVarForPathExpansion(name);
            if (!env.empty())
            {
                value.replace(pos, end - pos + 1, env);
                pos += env.size();
            }
            else
            {
                pos = end + 1;
            }
        }

        return NormalizePathSlashes(value);
    }

    bool ReadBatchDirListFile(const std::string& path, std::vector<std::string>& dirs)
    {
        const std::string listPath = ExpandEnvironmentVariablesInPath(path);
        std::ifstream in(listPath.c_str(), std::ios::binary);
        if (!in) return false;
        std::string line;
        while (std::getline(in, line))
        {
            std::string dir = StripInlineCommentAndTrim(line);
            if (dir.empty()) continue;
            if (dir.size() >= 2 &&
                ((dir.front() == '\"' && dir.back() == '\"') ||
                    (dir.front() == '\'' && dir.back() == '\'')))
            {
                dir = dir.substr(1, dir.size() - 2);
                dir = Trim(dir);
            }
            dir = ExpandEnvironmentVariablesInPath(dir);
            if (!dir.empty()) dirs.push_back(dir);
        }
        return true;
    }


    std::string CurrentWorkingDirectory()
    {
        char buf[4096];
#ifdef _WIN32
        if (_getcwd(buf, sizeof(buf)) != NULL) return NormalizePathSlashes(buf);
#else
        if (getcwd(buf, sizeof(buf)) != NULL) return NormalizePathSlashes(buf);
#endif
        return std::string();
    }

    std::vector<std::string> SplitPathParts(const std::string& path)
    {
        const std::string n = NormalizePathSlashes(path);
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start <= n.size())
        {
            const std::size_t slash = n.find('/', start);
            const std::string part = n.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (!part.empty() && part != ".") parts.push_back(part);
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return parts;
    }

    std::string PathRootToken(const std::string& path)
    {
        const std::string n = NormalizePathSlashes(path);
        if (n.size() >= 2 && std::isalpha(static_cast<unsigned char>(n[0])) && n[1] == ':')
        {
            std::string drive = n.substr(0, 2);
            std::transform(drive.begin(), drive.end(), drive.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return drive;
        }
        if (!n.empty() && n[0] == '/') return "/";
        return std::string();
    }

    std::string StripPathRootForParts(const std::string& path)
    {
        std::string n = NormalizePathSlashes(path);
        if (n.size() >= 2 && std::isalpha(static_cast<unsigned char>(n[0])) && n[1] == ':')
        {
            n.erase(0, 2);
            while (!n.empty() && n[0] == '/') n.erase(0, 1);
            return n;
        }
        while (!n.empty() && n[0] == '/') n.erase(0, 1);
        return n;
    }

    std::string CollapseDotDotPath(const std::string& path)
    {
        const std::string n = NormalizePathSlashes(path);
        const std::string root = PathRootToken(n);
        std::vector<std::string> in = SplitPathParts(StripPathRootForParts(n));
        std::vector<std::string> out;
        for (std::size_t i = 0; i < in.size(); ++i)
        {
            if (in[i] == "..")
            {
                if (!out.empty() && out.back() != "..") out.pop_back();
                else if (root.empty()) out.push_back(in[i]);
            }
            else
            {
                out.push_back(in[i]);
            }
        }

        std::ostringstream oss;
        if (root == "/")
        {
            oss << "/";
        }
        else if (!root.empty())
        {
            oss << root;
        }
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            std::string cur = oss.str();
            if (!cur.empty() && cur[cur.size() - 1] != '/') oss << "/";
            oss << out[i];
        }
        std::string r = oss.str();
        if (r.empty()) return root.empty() ? std::string(".") : root;
        return r;
    }

    std::string MakeAbsoluteLexicalPath(const std::string& path)
    {
        const std::string n = NormalizePathSlashes(path);
        if (IsAbsolutePath(n)) return CollapseDotDotPath(n);
        const std::string cwd = CurrentWorkingDirectory();
        if (cwd.empty()) return CollapseDotDotPath(n);
        return CollapseDotDotPath(JoinPath(cwd, n));
    }

    std::string MakeRelativePathLexical(const std::string& fromDir, const std::string& toPath)
    {
        const std::string fromAbs = MakeAbsoluteLexicalPath(fromDir);
        const std::string toAbs = MakeAbsoluteLexicalPath(toPath);

        const std::string fromRoot = PathRootToken(fromAbs);
        const std::string toRoot = PathRootToken(toAbs);
        if (fromRoot != toRoot) return NormalizePathSlashes(toPath);

        std::vector<std::string> fromParts = SplitPathParts(StripPathRootForParts(fromAbs));
        std::vector<std::string> toParts = SplitPathParts(StripPathRootForParts(toAbs));

        std::size_t common = 0;
        while (common < fromParts.size() && common < toParts.size())
        {
#ifdef _WIN32
            if (ToLowerAscii(fromParts[common]) != ToLowerAscii(toParts[common])) break;
#else
            if (fromParts[common] != toParts[common]) break;
#endif
            ++common;
        }

        std::vector<std::string> rel;
        for (std::size_t i = common; i < fromParts.size(); ++i) rel.push_back("..");
        for (std::size_t i = common; i < toParts.size(); ++i) rel.push_back(toParts[i]);

        if (rel.empty()) return FileNameOnly(toPath);
        std::ostringstream oss;
        for (std::size_t i = 0; i < rel.size(); ++i)
        {
            if (i) oss << "/";
            oss << rel[i];
        }
        return oss.str();
    }

    std::string MakeGeneratedHeaderIncludeText(const std::string& generatedHeader, const std::string& inputHeader)
    {
        const std::string genDir = DirectoryName(generatedHeader);
        if (genDir.empty()) return NormalizePathSlashes(inputHeader);
        return MakeRelativePathLexical(genDir, inputHeader);
    }


    std::string ReadWholeFileText(const std::string& path)
    {
        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in) return std::string();
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::string DecodeXmlEntities(std::string s)
    {
        struct Pair { const char* from; const char* to; } pairs[] = {
            {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}
        };
        for (std::size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i)
        {
            std::size_t pos = 0;
            while ((pos = s.find(pairs[i].from, pos)) != std::string::npos)
            {
                s.replace(pos, std::strlen(pairs[i].from), pairs[i].to);
                pos += std::strlen(pairs[i].to);
            }
        }
        return s;
    }

    std::vector<std::string> SplitSemicolonList(const std::string& text)
    {
        std::vector<std::string> out;
        std::string cur;
        bool inQuote = false;
        char quote = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if ((c == '"' || c == '\'') && (!inQuote || c == quote))
            {
                if (!inQuote) { inQuote = true; quote = c; }
                else { inQuote = false; quote = 0; }
                cur += c;
                continue;
            }
            if (c == ';' && !inQuote)
            {
                std::string item = Trim(cur);
                if (!item.empty()) out.push_back(item);
                cur.clear();
            }
            else
            {
                cur += c;
            }
        }
        std::string item = Trim(cur);
        if (!item.empty()) out.push_back(item);
        return out;
    }

    std::string StripOuterQuotes(std::string s)
    {
        s = Trim(s);
        if (s.size() >= 2 &&
            ((s.front() == '"' && s.back() == '"') ||
                (s.front() == '\'' && s.back() == '\'')))
        {
            return s.substr(1, s.size() - 2);
        }
        return s;
    }

    std::vector<std::string> TokenizeCommandLineLike(const std::string& text)
    {
        std::vector<std::string> out;
        std::string cur;
        bool inQuote = false;
        char quote = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if ((c == '"' || c == '\'') && (!inQuote || c == quote))
            {
                if (!inQuote) { inQuote = true; quote = c; }
                else { inQuote = false; quote = 0; }
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(c)) && !inQuote)
            {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            }
            else
            {
                cur += c;
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    std::vector<std::string> ExtractXmlTagValues(const std::string& block, const std::string& tag)
    {
        std::vector<std::string> out;
        const std::string open = "<" + tag;
        const std::string close = "</" + tag + ">";
        std::size_t pos = 0;
        while ((pos = block.find(open, pos)) != std::string::npos)
        {
            const std::size_t gt = block.find('>', pos);
            if (gt == std::string::npos) break;
            const std::size_t end = block.find(close, gt + 1);
            if (end == std::string::npos) break;
            out.push_back(DecodeXmlEntities(block.substr(gt + 1, end - gt - 1)));
            pos = end + close.size();
        }
        return out;
    }

    std::string ExtractXmlAttribute(const std::string& tagText, const std::string& attr)
    {
        const std::string key1 = attr + "=\"";
        const std::string key2 = attr + "='";
        std::size_t p = tagText.find(key1);
        char endQuote = '"';
        if (p != std::string::npos) p += key1.size();
        else
        {
            p = tagText.find(key2);
            if (p == std::string::npos) return std::string();
            p += key2.size();
            endQuote = '\'';
        }
        const std::size_t e = tagText.find(endQuote, p);
        if (e == std::string::npos) return std::string();
        return DecodeXmlEntities(tagText.substr(p, e - p));
    }

    bool ConditionMatchesConfigPlatform(const std::string& startTag, const std::string& config, const std::string& platform)
    {
        const std::string cond = ExtractXmlAttribute(startTag, "Condition");
        if (cond.empty()) return true;
        if (config.empty() && platform.empty()) return true;
        const std::string want = config + "|" + platform;
        return cond.find(want) != std::string::npos;
    }

    std::vector<std::string> ExtractMatchingBlocks(const std::string& xml,
        const std::string& tag,
        const std::string& config,
        const std::string& platform)
    {
        std::vector<std::string> out;
        const std::string open = "<" + tag;
        const std::string close = "</" + tag + ">";
        std::size_t pos = 0;
        while ((pos = xml.find(open, pos)) != std::string::npos)
        {
            const std::size_t gt = xml.find('>', pos);
            if (gt == std::string::npos) break;
            const std::size_t end = xml.find(close, gt + 1);
            if (end == std::string::npos) break;
            const std::string startTag = xml.substr(pos, gt - pos + 1);
            if (ConditionMatchesConfigPlatform(startTag, config, platform))
            {
                out.push_back(xml.substr(gt + 1, end - gt - 1));
            }
            pos = end + close.size();
        }
        return out;
    }

    std::string EnsureTrailingSlash(std::string path)
    {
        path = NormalizePathSlashes(path);
        if (!path.empty() && path[path.size() - 1] != '/') path += '/';
        return path;
    }

    std::string GetEnvVarString(const std::string& name)
    {
        const char* v = std::getenv(name.c_str());
        return v ? std::string(v) : std::string();
    }

    void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
    {
        if (from.empty()) return;
        std::size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos)
        {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    std::string ExpandMsbuildMacros(std::string value,
        const std::string& projectDir,
        const std::string& config,
        const std::string& platform)
    {
        const std::string projectDirSlash = EnsureTrailingSlash(projectDir);
        const std::string parentDirSlash = EnsureTrailingSlash(DirectoryName(projectDir));
        ReplaceAll(value, "$(ProjectDir)", projectDirSlash);
        ReplaceAll(value, "$(MSBuildProjectDirectory)", NormalizePathSlashes(projectDir));
        ReplaceAll(value, "$(MSBuildThisFileDirectory)", projectDirSlash);
        ReplaceAll(value, "$(SolutionDir)", parentDirSlash);
        ReplaceAll(value, "$(Configuration)", config);
        ReplaceAll(value, "$(Platform)", platform);
        ReplaceAll(value, "$(PlatformTarget)", platform);
        ReplaceAll(value, "$(OutDir)", JoinPath(projectDir, platform + "/" + config + "/"));
        ReplaceAll(value, "$(IntDir)", JoinPath(projectDir, "obj/" + platform + "/" + config + "/"));

        // Best-effort expansion for simple environment-style MSBuild macros, e.g. $(LLVM_ROOT).
        std::size_t pos = 0;
        while ((pos = value.find("$(", pos)) != std::string::npos)
        {
            const std::size_t end = value.find(')', pos + 2);
            if (end == std::string::npos) break;
            const std::string name = value.substr(pos + 2, end - pos - 2);
            const std::string env = GetEnvVarString(name);
            if (!env.empty())
            {
                value.replace(pos, end - pos + 1, NormalizePathSlashes(env));
                pos += env.size();
            }
            else
            {
                pos = end + 1;
            }
        }
        value = ExpandEnvironmentVariablesInPath(value);
        return NormalizePathSlashes(value);
    }

    bool ContainsUnexpandedMacro(const std::string& s)
    {
        return s.find("$(") != std::string::npos ||
            s.find("${") != std::string::npos ||
            s.find("%(") != std::string::npos;
    }

    bool ContainsUnexpandedPercentEnv(const std::string& s)
    {
        const std::size_t first = s.find('%');
        if (first == std::string::npos) return false;
        const std::size_t second = s.find('%', first + 1);
        return second != std::string::npos;
    }

    bool ContainsAnyUnexpandedVariable(const std::string& s)
    {
        return ContainsUnexpandedMacro(s) || ContainsUnexpandedPercentEnv(s);
    }

    bool IsClangSeparateValueOption(const std::string& arg)
    {
        return arg == "-include" || arg == "--include" ||
            arg == "-I" || arg == "/I" ||
            arg == "-isystem" || arg == "-imsvc" ||
            arg == "-idirafter" || arg == "-iquote" ||
            arg == "-D" || arg == "/D" ||
            arg == "-U" || arg == "/U" ||
            arg == "-x" || arg == "/FI";
    }

    bool IsPathLikeClangArg(const std::string& arg)
    {
        if (arg.empty()) return false;
        if (arg.find('/') != std::string::npos || arg.find('\\') != std::string::npos) return true;
        const std::size_t dot = arg.find_last_of('.');
        if (dot != std::string::npos)
        {
            const std::string ext = ToLowerAscii(arg.substr(dot));
            if (ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx" ||
                ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx") return true;
        }
        return false;
    }

    std::string ExpandUserClangArg(std::string arg)
    {
        arg = StripOuterQuotes(arg);
        arg = ExpandEnvironmentVariablesInPath(arg);
        return StripOuterQuotes(arg);
    }

    void AddRawClangArg(std::vector<std::string>& args, const std::string& arg)
    {
        if (!arg.empty()) args.push_back(arg);
    }

    void AddExpandedUserClangArgs(std::vector<std::string>& merged, const std::vector<std::string>& userArgs)
    {
        for (std::size_t i = 0; i < userArgs.size(); ++i)
        {
            std::string a = ExpandUserClangArg(userArgs[i]);
            if (a.empty()) continue;

            if (a == "--include")
            {
                std::cerr << "[clang args] translate unsupported --include to -include\n";
                a = "-include";
            }
            else if (a.compare(0, 10, "--include=") == 0)
            {
                std::string v = ExpandUserClangArg(a.substr(10));
                std::cerr << "[clang args] translate unsupported --include= to -include: " << v << "\n";
                if (ContainsAnyUnexpandedVariable(v))
                {
                    std::cerr << "[clang args] warning: unresolved variable in include argument: " << v << "\n";
                }
                AddRawClangArg(merged, "-include");
                AddRawClangArg(merged, NormalizePathSlashes(v));
                continue;
            }

            if ((a == "-include" || a == "/FI") && i + 1 < userArgs.size())
            {
                std::string v = ExpandUserClangArg(userArgs[++i]);
                if (ContainsAnyUnexpandedVariable(v))
                {
                    std::cerr << "[clang args] warning: unresolved variable in forced include: " << v << "\n";
                }
                AddRawClangArg(merged, "-include");
                AddRawClangArg(merged, NormalizePathSlashes(v));
                continue;
            }

            if ((a == "-I" || a == "/I" || a == "-isystem" || a == "-imsvc" ||
                a == "-idirafter" || a == "-iquote") && i + 1 < userArgs.size())
            {
                std::string v = ExpandUserClangArg(userArgs[++i]);
                if (ContainsAnyUnexpandedVariable(v))
                {
                    std::cerr << "[clang args] warning: unresolved variable in include directory: " << v << "\n";
                }
                AddRawClangArg(merged, a == "/I" ? "-I" : a);
                AddRawClangArg(merged, NormalizePathSlashes(v));
                continue;
            }

            if ((a == "-D" || a == "/D" || a == "-U" || a == "/U" || a == "-x") && i + 1 < userArgs.size())
            {
                std::string v = ExpandUserClangArg(userArgs[++i]);
                AddRawClangArg(merged, (a == "/D") ? "-D" : ((a == "/U") ? "-U" : a));
                AddRawClangArg(merged, v);
                continue;
            }

            if (a.compare(0, 2, "/I") == 0 && a.size() > 2)
            {
                std::string v = ExpandUserClangArg(a.substr(2));
                AddRawClangArg(merged, "-I" + NormalizePathSlashes(v));
                continue;
            }
            if (a.compare(0, 2, "-I") == 0 && a.size() > 2)
            {
                std::string v = ExpandUserClangArg(a.substr(2));
                AddRawClangArg(merged, "-I" + NormalizePathSlashes(v));
                continue;
            }
            if (a.compare(0, 3, "/FI") == 0 && a.size() > 3)
            {
                std::string v = ExpandUserClangArg(a.substr(3));
                AddRawClangArg(merged, "-include");
                AddRawClangArg(merged, NormalizePathSlashes(v));
                continue;
            }
            if (a.compare(0, 2, "/D") == 0 && a.size() > 2)
            {
                AddRawClangArg(merged, "-D" + a.substr(2));
                continue;
            }

            if (ContainsAnyUnexpandedVariable(a))
            {
                std::cerr << "[clang args] warning: unresolved variable in argument: " << a << "\n";
            }
            if (!a.empty() && a[0] != '-' && a[0] != '/' && IsPathLikeClangArg(a))
            {
                std::cerr << "[clang args] warning: bare path-like clang argument will be treated as an input file: " << a << "\n";
                std::cerr << "[clang args] hint: use -include <file> for forced include, or -I<dir> for include directory.\n";
            }
            AddRawClangArg(merged, a);
        }
    }

    bool HasArgPrefix(const std::vector<std::string>& args, const std::string& prefix)
    {
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            if (args[i].compare(0, prefix.size(), prefix) == 0) return true;
        }
        return false;
    }

    bool HasExactArg(const std::vector<std::string>& args, const std::string& exact)
    {
        return std::find(args.begin(), args.end(), exact) != args.end();
    }

    void AddUniqueArg(std::vector<std::string>& args, const std::string& arg)
    {
        if (arg.empty()) return;
        if (std::find(args.begin(), args.end(), arg) == args.end()) args.push_back(arg);
    }

    std::string MapLanguageStandardToClang(const std::string& value)
    {
        std::string v = Trim(value);
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (v == "stdcpp14") return "-std=c++14";
        if (v == "stdcpp17") return "-std=c++17";
        if (v == "stdcpp20") return "-std=c++20";
        if (v == "stdcpp23") return "-std=c++23";
        if (v == "stdcpplatest") return "-std=c++20";
        if (v == "stdcpp11") return "-std=c++11";
        if (v.find("c++") != std::string::npos)
        {
            if (v.compare(0, 5, "-std=") == 0) return value;
            if (v.compare(0, 5, "/std:") == 0) return "-std=" + v.substr(5);
        }
        return std::string();
    }

    void AddIncludeDirArg(std::vector<std::string>& args, std::string dir, const std::string& projectDir,
        const std::string& config, const std::string& platform)
    {
        dir = StripOuterQuotes(ExpandMsbuildMacros(dir, projectDir, config, platform));
        if (dir.empty()) return;
        if (ContainsUnexpandedMacro(dir))
        {
            std::cerr << "[vcxproj] skip unresolved include dir: " << dir << "\n";
            return;
        }
        AddUniqueArg(args, "-I" + dir);
    }

    void AddDefineArg(std::vector<std::string>& args, std::string def, const std::string& projectDir,
        const std::string& config, const std::string& platform)
    {
        def = StripOuterQuotes(ExpandMsbuildMacros(def, projectDir, config, platform));
        if (def.empty()) return;
        if (def.find("%(") != std::string::npos) return;
        if (ContainsUnexpandedMacro(def))
        {
            std::cerr << "[vcxproj] skip unresolved preprocessor definition: " << def << "\n";
            return;
        }
        AddUniqueArg(args, "-D" + def);
    }

    void AddForcedIncludeArg(std::vector<std::string>& args, std::string file, const std::string& projectDir,
        const std::string& config, const std::string& platform)
    {
        file = StripOuterQuotes(ExpandMsbuildMacros(file, projectDir, config, platform));
        if (file.empty()) return;
        if (ContainsUnexpandedMacro(file))
        {
            std::cerr << "[vcxproj] skip unresolved forced include: " << file << "\n";
            return;
        }
        args.push_back("-include");
        args.push_back(file);
    }

    void ParseAdditionalOptionsIntoArgs(std::vector<std::string>& args,
        const std::string& text,
        const std::string& projectDir,
        const std::string& config,
        const std::string& platform)
    {
        const std::vector<std::string> toks = TokenizeCommandLineLike(DecodeXmlEntities(text));
        for (std::size_t i = 0; i < toks.size(); ++i)
        {
            std::string t = toks[i];
            if (t.empty() || t.find("%(") != std::string::npos) continue;
            if (t.compare(0, 2, "/I") == 0 && t.size() > 2)
            {
                AddIncludeDirArg(args, t.substr(2), projectDir, config, platform);
            }
            else if ((t == "/I" || t == "-I") && i + 1 < toks.size())
            {
                AddIncludeDirArg(args, toks[++i], projectDir, config, platform);
            }
            else if (t.compare(0, 2, "/D") == 0 && t.size() > 2)
            {
                AddDefineArg(args, t.substr(2), projectDir, config, platform);
            }
            else if ((t == "/D" || t == "-D") && i + 1 < toks.size())
            {
                AddDefineArg(args, toks[++i], projectDir, config, platform);
            }
            else if (t.compare(0, 3, "/FI") == 0 && t.size() > 3)
            {
                AddForcedIncludeArg(args, t.substr(3), projectDir, config, platform);
            }
            else if ((t == "/FI" || t == "-include") && i + 1 < toks.size())
            {
                AddForcedIncludeArg(args, toks[++i], projectDir, config, platform);
            }
            else if (t.compare(0, 5, "/std:") == 0)
            {
                const std::string stdarg = MapLanguageStandardToClang(t);
                if (!stdarg.empty()) AddUniqueArg(args, stdarg);
            }
        }
    }

    bool ApplyVcxprojSettings(Options& opt)
    {
        std::vector<std::string> merged;
        AddUniqueArg(merged, "-x");
        AddUniqueArg(merged, "c++");

        std::string detectedStd;
        if (!opt.vcxprojPath.empty())
        {
            const std::string vcxproj = NormalizePathSlashes(ExpandEnvironmentVariablesInPath(opt.vcxprojPath));
            const std::string xml = ReadWholeFileText(vcxproj);
            if (xml.empty())
            {
                std::cerr << "Failed to read vcxproj: " << opt.vcxprojPath << "\n";
                return false;
            }
            const std::string projectDir = DirectoryName(vcxproj);
            const std::vector<std::string> itemGroups = ExtractMatchingBlocks(xml, "ItemDefinitionGroup", opt.configuration, opt.platform);
            std::vector<std::string> blocks = itemGroups;
            // Some minimal projects put ClCompile settings outside ItemDefinitionGroup.
            // Only fall back to the whole file when no matching ItemDefinitionGroup exists;
            // otherwise using the whole file would mix Debug/Release and x86/x64 settings.
            if (blocks.empty()) blocks.push_back(xml);

            for (std::size_t bi = 0; bi < blocks.size(); ++bi)
            {
                const std::string& block = blocks[bi];
                const std::vector<std::string> incs = ExtractXmlTagValues(block, "AdditionalIncludeDirectories");
                for (std::size_t i = 0; i < incs.size(); ++i)
                {
                    const std::vector<std::string> parts = SplitSemicolonList(incs[i]);
                    for (std::size_t j = 0; j < parts.size(); ++j) AddIncludeDirArg(merged, parts[j], projectDir, opt.configuration, opt.platform);
                }

                const std::vector<std::string> defs = ExtractXmlTagValues(block, "PreprocessorDefinitions");
                for (std::size_t i = 0; i < defs.size(); ++i)
                {
                    const std::vector<std::string> parts = SplitSemicolonList(defs[i]);
                    for (std::size_t j = 0; j < parts.size(); ++j) AddDefineArg(merged, parts[j], projectDir, opt.configuration, opt.platform);
                }

                const std::vector<std::string> forced = ExtractXmlTagValues(block, "ForcedIncludeFiles");
                for (std::size_t i = 0; i < forced.size(); ++i)
                {
                    const std::vector<std::string> parts = SplitSemicolonList(forced[i]);
                    for (std::size_t j = 0; j < parts.size(); ++j) AddForcedIncludeArg(merged, parts[j], projectDir, opt.configuration, opt.platform);
                }

                const std::vector<std::string> standards = ExtractXmlTagValues(block, "LanguageStandard");
                for (std::size_t i = 0; i < standards.size(); ++i)
                {
                    const std::string mapped = MapLanguageStandardToClang(standards[i]);
                    if (!mapped.empty()) detectedStd = mapped;
                }

                const std::vector<std::string> extra = ExtractXmlTagValues(block, "AdditionalOptions");
                for (std::size_t i = 0; i < extra.size(); ++i)
                {
                    ParseAdditionalOptionsIntoArgs(merged, extra[i], projectDir, opt.configuration, opt.platform);
                }
            }
        }

        // User-supplied arguments after "--" win by appearing later.
        // Keep option/value pairs such as -include <file> intact. Do NOT use
        // AddUniqueArg for -include itself: multiple forced includes are legal,
        // and deduplicating only the marker turns the following filename into a
        // stray input file. Environment variables in user clang args are expanded
        // here as well, e.g. %AQARCH%\cmodel\inc\aqcm.hpp.
        AddExpandedUserClangArgs(merged, opt.clangArgs);

        if (!HasArgPrefix(merged, "-std=") && !HasArgPrefix(merged, "/std:"))
        {
            if (!detectedStd.empty()) AddUniqueArg(merged, detectedStd);
            else AddUniqueArg(merged, "-std=c++14");
        }

        opt.clangArgs.swap(merged);

        std::cout << "[vcxproj] effective libclang args prepared.\n";
        PrintClangArgsForLog(opt.clangArgs);
        return true;
    }

    std::string MakeFlatReflectAutoFileName(const std::string& input, const std::string& root)
    {
        // Batch outputs are intentionally flattened into opt.outputHeader.
        // The generated file name still carries the input header's path relative
        // to its scanned root, so headers with the same basename in different
        // subdirectories do not overwrite each other.
        //
        // Examples, assuming root = SystemCReflectTest:
        //   SystemCReflectTest/gpu_demo.h        -> gpu_demo_reflect_auto.h
        //   SystemCReflectTest/modules/sm.h      -> modules_sm_reflect_auto.h
        //   SystemCReflectTest/trace/bus.trace.h -> trace_bus_trace_reflect_auto.h
        const std::string rel = NormalizePathSlashes(MakeRelativeToRoot(input, root));
        const std::size_t dot = rel.find_last_of('.');

        std::string stem = (dot == std::string::npos) ? rel : rel.substr(0, dot);
        std::string ext = (dot == std::string::npos) ? std::string(".h") : rel.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".h" && ext != ".hpp" && ext != ".hh" && ext != ".hxx") ext = ".h";

        std::string flat;
        flat.reserve(stem.size() + 24);
        bool lastWasUnderscore = false;
        for (std::size_t i = 0; i < stem.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(stem[i]);
            const bool ok = std::isalnum(c) || c == '_';
            const char out = ok ? static_cast<char>(c) : '_';
            if (out == '_')
            {
                if (!lastWasUnderscore) flat += out;
                lastWasUnderscore = true;
            }
            else
            {
                flat += out;
                lastWasUnderscore = false;
            }
        }
        while (!flat.empty() && flat.front() == '_') flat.erase(flat.begin());
        while (!flat.empty() && flat.back() == '_') flat.erase(flat.size() - 1);
        if (flat.empty()) flat = "generated";
        if (std::isdigit(static_cast<unsigned char>(flat[0]))) flat.insert(flat.begin(), '_');

        return flat + "_reflect_auto" + ext;
    }

    bool MakeDirectoryOne(const std::string& dir)
    {
        if (dir.empty()) return true;
#ifdef _WIN32
        if (_mkdir(dir.c_str()) == 0) return true;
#else
        if (mkdir(dir.c_str(), 0755) == 0) return true;
#endif
        struct stat st;
#ifdef _WIN32
        return stat(dir.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR);
#else
        return stat(dir.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
#endif
    }

    bool MakeDirectoryRecursive(const std::string& dir)
    {
        std::string n = NormalizePathSlashes(dir);
        if (n.empty()) return true;
        std::string cur;
        std::size_t start = 0;
        if (n.size() >= 2 && n[1] == ':')
        {
            cur = n.substr(0, 2);
            start = 2;
            if (start < n.size() && n[start] == '/') { cur += '/'; ++start; }
        }
        else if (!n.empty() && n[0] == '/')
        {
            cur = "/";
            start = 1;
        }

        while (start <= n.size())
        {
            const std::size_t slash = n.find('/', start);
            const std::string part = n.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (!part.empty())
            {
                if (!cur.empty() && cur[cur.size() - 1] != '/') cur += '/';
                cur += part;
                if (!MakeDirectoryOne(cur)) return false;
            }
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return true;
    }


    bool IsSameCanonicalPath(const std::string& a, const std::string& b)
    {
        return CanonicalSourcePathKey(a) == CanonicalSourcePathKey(b);
    }

    bool IsCanonicalAncestorOrSelf(const std::string& ancestorRaw, const std::string& childRaw)
    {
        std::string ancestor = CanonicalSourcePathKey(ancestorRaw);
        std::string child = CanonicalSourcePathKey(childRaw);
        if (ancestor.empty() || child.empty()) return false;
        if (ancestor == child) return true;
        if (!ancestor.empty() && ancestor[ancestor.size() - 1] != '/') ancestor += '/';
        return child.compare(0, ancestor.size(), ancestor) == 0;
    }

    bool IsRootLikePathForClear(const std::string& path)
    {
        const std::string p = NormalizePathSlashes(MakeAbsoluteLexicalPath(path));
        if (p.empty()) return true;
        if (p == "/") return true;
        if (p.size() == 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') return true;
        if (p.size() == 3 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':' && p[2] == '/') return true;
        return false;
    }

    bool IsSafeBatchOutputDirectoryToClear(const std::string& outputDir,
        const std::vector<std::string>& roots,
        std::string& reason)
    {
        reason.clear();
        if (outputDir.empty())
        {
            reason = "empty output directory";
            return false;
        }

        const std::string absOutput = NormalizePathSlashes(MakeAbsoluteLexicalPath(outputDir));
        if (IsRootLikePathForClear(absOutput))
        {
            reason = "output directory resolves to a filesystem root";
            return false;
        }

        const std::string cwd = NormalizePathSlashes(CurrentWorkingDirectory());
        if (!cwd.empty() && IsSameCanonicalPath(absOutput, cwd))
        {
            reason = "output directory resolves to the current working directory";
            return false;
        }

        for (std::size_t i = 0; i < roots.size(); ++i)
        {
            const std::string root = NormalizePathSlashes(MakeAbsoluteLexicalPath(roots[i]));
            if (root.empty()) continue;
            if (IsSameCanonicalPath(absOutput, root))
            {
                reason = "output directory is the same as a batch input root: " + root;
                return false;
            }
            if (IsCanonicalAncestorOrSelf(absOutput, root))
            {
                reason = "output directory is an ancestor of a batch input root: " + root;
                return false;
            }
        }
        return true;
    }

    bool ShouldPreservePathDuringClear(const std::set<std::string>& preservePaths, const std::string& path)
    {
        if (preservePaths.empty()) return false;
        return preservePaths.find(CanonicalSourcePathKey(path)) != preservePaths.end();
    }

    bool ClearDirectoryContentsRecursive(const std::string& dir,
        const std::set<std::string>& preservePaths,
        const std::string& label)
    {
        const std::string normalizedDir = NormalizePathSlashes(dir);
#ifdef _WIN32
        WIN32_FIND_DATAA data;
        const std::string pattern = JoinPath(normalizedDir, "*");
        HANDLE h = FindFirstFileA(pattern.c_str(), &data);
        if (h == INVALID_HANDLE_VALUE)
        {
            const DWORD gle = GetLastError();
            if (gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND) return true;
            PrintPathFailureContext(label + " scan failed", normalizedDir);
            return false;
        }

        bool ok = true;
        do
        {
            const std::string name = data.cFileName;
            if (name == "." || name == "..") continue;
            const std::string full = JoinPath(normalizedDir, name);
            if (ShouldPreservePathDuringClear(preservePaths, full))
            {
                if (gDebugAst) std::cout << "[batch output clear] preserve: " << full << "\n";
                continue;
            }

            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!ClearDirectoryContentsRecursive(full, preservePaths, label)) ok = false;
                SetFileAttributesA(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!RemoveDirectoryA(full.c_str()))
                {
                    PrintPathFailureContext(label + " remove directory failed", full);
                    ok = false;
                }
            }
            else
            {
                SetFileAttributesA(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileA(full.c_str()))
                {
                    PrintPathFailureContext(label + " delete file failed", full);
                    ok = false;
                }
            }
        } while (FindNextFileA(h, &data));

        const DWORD finalError = GetLastError();
        FindClose(h);
        if (finalError != ERROR_NO_MORE_FILES)
        {
            PrintPathFailureContext(label + " scan iteration failed", normalizedDir);
            ok = false;
        }
        return ok;
#else
        DIR* d = opendir(normalizedDir.c_str());
        if (!d)
        {
            if (errno == ENOENT) return true;
            PrintPathFailureContext(label + " scan failed", normalizedDir);
            return false;
        }

        bool ok = true;
        while (dirent* ent = readdir(d))
        {
            const std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            const std::string full = JoinPath(normalizedDir, name);
            if (ShouldPreservePathDuringClear(preservePaths, full))
            {
                if (gDebugAst) std::cout << "[batch output clear] preserve: " << full << "\n";
                continue;
            }

            struct stat st;
            if (lstat(full.c_str(), &st) != 0)
            {
                PrintPathFailureContext(label + " stat failed", full);
                ok = false;
                continue;
            }
            if (S_ISDIR(st.st_mode))
            {
                if (!ClearDirectoryContentsRecursive(full, preservePaths, label)) ok = false;
                if (rmdir(full.c_str()) != 0)
                {
                    PrintPathFailureContext(label + " remove directory failed", full);
                    ok = false;
                }
            }
            else
            {
                if (unlink(full.c_str()) != 0)
                {
                    PrintPathFailureContext(label + " delete file failed", full);
                    ok = false;
                }
            }
        }
        closedir(d);
        return ok;
#endif
    }

    bool ClearBatchOutputDirectoryBeforeEmit(const std::string& outputDir,
        const std::vector<std::string>& roots,
        const Options& opt)
    {
        std::string reason;
        if (!IsSafeBatchOutputDirectoryToClear(outputDir, roots, reason))
        {
            std::cerr << "[batch output clear refused] " << reason << "\n";
            std::cerr << "[batch output clear refused] outputDir=" << outputDir << "\n";
            return false;
        }

        std::set<std::string> preservePaths;
        if (!opt.logFile.empty())
        {
            const std::string logPath = NormalizePathSlashes(MakeAbsoluteLexicalPath(ExpandEnvironmentVariablesInPath(StripOuterQuotes(opt.logFile))));
            if (!logPath.empty()) preservePaths.insert(CanonicalSourcePathKey(logPath));
        }

        if (!DirectoryExistsForDiagnostics(outputDir))
        {
            std::cout << "[batch output clear] output directory does not exist yet: " << outputDir << "\n";
            return true;
        }

        std::cout << "[batch output clear] clearing output directory before emission: " << outputDir << "\n";
        if (!ClearDirectoryContentsRecursive(outputDir, preservePaths, "[batch output clear]"))
        {
            std::cerr << "[batch output clear failed] outputDir=" << outputDir << "\n";
            return false;
        }
        return true;
    }

    void CollectHeaderFilesInDirectory(const std::string& root, bool recursive, bool hOrHppOnly, std::vector<std::string>& out)
    {
        const std::string dir = NormalizePathSlashes(root);
#ifdef _WIN32
        WIN32_FIND_DATAA data;
        const std::string pattern = JoinPath(dir, "*");
        HANDLE h = FindFirstFileA(pattern.c_str(), &data);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            const std::string name = data.cFileName;
            if (name == "." || name == "..") continue;
            const std::string full = JoinPath(dir, name);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (recursive) CollectHeaderFilesInDirectory(full, recursive, hOrHppOnly, out);
            }
            else if (hOrHppOnly ? HasHOrHppExtension(full) : HasHeaderExtension(full))
            {
                out.push_back(full);
            }
        } while (FindNextFileA(h, &data));
        FindClose(h);
#else
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        while (dirent* ent = readdir(d))
        {
            const std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            const std::string full = JoinPath(dir, name);
            struct stat st;
            if (stat(full.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode))
            {
                if (recursive) CollectHeaderFilesInDirectory(full, recursive, hOrHppOnly, out);
            }
            else if (S_ISREG(st.st_mode) && (hOrHppOnly ? HasHOrHppExtension(full) : HasHeaderExtension(full)))
            {
                out.push_back(full);
            }
        }
        closedir(d);
#endif
    }

    std::string MakeAggregateIncludePath(const std::string& aggregateHeader, const std::string& generatedHeader)
    {
        const std::string aggDir = DirectoryName(aggregateHeader);
        const std::string gen = NormalizePathSlashes(generatedHeader);
        if (!aggDir.empty() && StartsWithPathPrefix(gen, aggDir))
        {
            return MakeRelativeToRoot(gen, aggDir);
        }
        return gen;
    }

    std::string BuildWaveRegistrationFunctionName(const Options& opt)
    {
        return "register_wave_dynamic_types_" + GetOutputStem(opt);
    }

    std::string BuildConvenienceRegistrationFunctionName(const Options& opt)
    {
        return "register_generated_reflection_" + GetOutputStem(opt);
    }


    int QualifiedNameDepth(const std::string& qname)
    {
        int depth = 0;
        for (std::size_t pos = qname.find("::"); pos != std::string::npos; pos = qname.find("::", pos + 2))
        {
            ++depth;
        }
        return depth;
    }

    std::string MakeReflectAccessAliasName(const RecordInfo& rec)
    {
        std::string name = rec.simpleName.empty() ? rec.qualifiedName : rec.simpleName;
        return "__reflect_nested_" + SanitizeIdentifier(name);
    }

    bool EmitGeneratedFile(const std::vector<RecordInfo>& records, const Options& opt, const std::map<std::string, const RecordInfo*>* globalByName = NULL)
    {
        errno = 0;
        std::ofstream os(opt.outputHeader.c_str(), std::ios::binary);
        if (!os)
        {
            PrintFileOpenFailure("[emit generated file error]", opt.outputHeader);
            return false;
        }

        std::map<std::string, const RecordInfo*> byName;
        if (globalByName) byName = *globalByName;
        for (std::size_t i = 0; i < records.size(); ++i)
        {
            byName[CanonicalRecordLookupKey(records[i].qualifiedName)] = &records[i];
            byName[CanonicalRecordLookupKey(records[i].simpleName)] = &records[i];
            if (!records[i].explicitReflectedType.empty()) byName[CanonicalRecordLookupKey(records[i].explicitReflectedType)] = &records[i];
        }

        // Emit parent records before nested records.  Private/protected nested types
        // are named through public aliases emitted inside wave::ReflectAccess<Parent>,
        // so the parent access class must be defined first.
        std::vector<const RecordInfo*> emitOrder;
        emitOrder.reserve(records.size());
        for (std::size_t i = 0; i < records.size(); ++i) emitOrder.push_back(&records[i]);
        std::stable_sort(emitOrder.begin(), emitOrder.end(), [](const RecordInfo* a, const RecordInfo* b) {
            const int da = QualifiedNameDepth(a->qualifiedName);
            const int db = QualifiedNameDepth(b->qualifiedName);
            if (da != db) return da < db;
            return a->qualifiedName < b->qualifiedName;
        });

        std::map<std::string, std::string> emittedTypeByQName;
        std::map<std::string, std::string> aliasNameByQName;
        std::map<std::string, std::vector<const RecordInfo*> > aliasChildrenByOwner;

        for (std::size_t i = 0; i < emitOrder.size(); ++i)
        {
            const RecordInfo& rec = *emitOrder[i];
            bool useAlias = false;
            if (!rec.accessPathPublic &&
                rec.templateKind == RecordTemplateKind::None &&
                !rec.semanticParentQualifiedName.empty() &&
                byName.find(CanonicalRecordLookupKey(rec.semanticParentQualifiedName)) != byName.end())
            {
                useAlias = true;
            }

            if (useAlias)
            {
                const std::map<std::string, std::string>::const_iterator pit = emittedTypeByQName.find(rec.semanticParentQualifiedName);
                if (pit != emittedTypeByQName.end())
                {
                    const std::string aliasName = MakeReflectAccessAliasName(rec);
                    aliasNameByQName[rec.qualifiedName] = aliasName;
                    aliasChildrenByOwner[rec.semanticParentQualifiedName].push_back(&rec);
                    emittedTypeByQName[rec.qualifiedName] = "::wave::ReflectAccess<" + pit->second + ">::" + aliasName;
                }
                else
                {
                    // Fallback should be rare; keep generation possible rather than
                    // producing an empty type.  If this path is hit, debug logs will show
                    // the inaccessible type and the user can add the parent to roots.
                    emittedTypeByQName[rec.qualifiedName] = BuildReflectedType(rec);
                }
            }
            else
            {
                emittedTypeByQName[rec.qualifiedName] = BuildReflectedType(rec);
            }
        }

        const std::string guard = MakeHeaderGuard(opt.outputHeader);
        os << "#ifndef " << guard << "\n";
        os << "#define " << guard << "\n\n";
        os << "#ifdef new\n";
        os << "#undef new\n";
        os << "#endif\n";
        os << "#ifdef make_shared\n";
        os << "#undef make_shared\n";
        os << "#endif\n";
        os << "#ifdef make_unique\n";
        os << "#undef make_unique\n";
        os << "#endif\n";
        os << "#include \"reflect_runtime.h\"\n";
        os << "namespace reflect { template <typename T> struct reflected_visitor; }\n";
        os << "namespace wave { template <typename T> struct ReflectAccess; }\n";
        os << "#include \"" << opt.headerInclude << "\"\n\n";

        os << "namespace wave {\n\n";
        for (std::size_t oi = 0; oi < emitOrder.size(); ++oi)
        {
            const RecordInfo& rec = *emitOrder[oi];
            const std::string recType = emittedTypeByQName[rec.qualifiedName];
            const std::vector<EmittedMember> members = BuildFlattenedMembers(opt, rec, byName);

            os << BuildClassSpecializationPrefix(rec);
            os << "struct ReflectAccess<" << recType << ">\n";
            os << "{\n";
            os << "    typedef " << recType << " Self;\n";

            const std::map<std::string, std::vector<const RecordInfo*> >::const_iterator ait = aliasChildrenByOwner.find(rec.qualifiedName);
            if (ait != aliasChildrenByOwner.end())
            {
                for (std::size_t ai = 0; ai < ait->second.size(); ++ai)
                {
                    const RecordInfo& child = *ait->second[ai];
                    const std::string aliasName = aliasNameByQName[child.qualifiedName];
                    if (rec.templateKind == RecordTemplateKind::Primary)
                    {
                        os << "    typedef typename Self::" << child.simpleName << " " << aliasName << ";\n";
                    }
                    else
                    {
                        os << "    typedef Self::" << child.simpleName << " " << aliasName << ";\n";
                    }
                }
                os << "\n";
            }

            for (std::size_t i = 0; i < members.size(); ++i)
            {
                const EmittedMember& m = members[i];
                if (!m.isBitField) continue;
                const std::string getterName = BuildBitGetterName(m.displayName);
                os << "    static " << m.typeName << " " << getterName << "(const Self* obj)\n";
                os << "    {\n";
                os << "        return static_cast<" << m.typeName << ">(" << m.constExpr << ");\n";
                os << "    }\n\n";
            }

            os << "    template <typename PtrVisitor, typename ValueVisitor, typename GetterVisitor>\n";
            os << "    static void visit(const Self* obj, PtrVisitor&& on_ptr, ValueVisitor&& on_value, GetterVisitor&& on_getter)\n";
            os << "    {\n";
            bool usedGetterConst = false;
            for (std::size_t i = 0; i < members.size(); ++i)
            {
                const EmittedMember& m = members[i];
                if (m.isBitField)
                {
                    usedGetterConst = true;
                    const std::string getterName = BuildBitGetterName(m.displayName);
                    os << "        on_getter(\"" << EscapeString(m.displayName) << "\", &::wave::ReflectAccess<" << recType << ">::" << getterName << ", " << m.bitWidth << ");\n";
                }
                else
                {
                    if (m.asBoolStorage)
                    {
                        os << "        on_ptr(\"" << EscapeString(m.displayName) << "\", ::wave::as_bool_storage_ptr(std::addressof(" << m.constExpr << ")));\n";
                    }
                    else if (m.exprIsPointerAlready)
                    {
                        os << "        on_ptr(\"" << EscapeString(m.displayName) << "\", " << m.constExpr << ");\n";
                    }
                    else
                    {
                        os << "        on_ptr(\"" << EscapeString(m.displayName) << "\", std::addressof(" << m.constExpr << "));\n";
                    }
                }
            }
            os << "        (void)on_value;\n";
            if (!usedGetterConst) os << "        (void)on_getter;\n";
            os << "    }\n\n";

            os << "    template <typename PtrVisitor, typename ValueVisitor>\n";
            os << "    static void visit(const Self* obj, PtrVisitor&& on_ptr, ValueVisitor&& on_value)\n";
            os << "    {\n";
            os << "        visit(obj, std::forward<PtrVisitor>(on_ptr), std::forward<ValueVisitor>(on_value), [](const char*, auto) {});\n";
            os << "    }\n\n";

            os << "    template <typename PtrVisitor, typename ValueVisitor, typename GetterVisitor>\n";
            os << "    static void visit(Self* obj, PtrVisitor&& on_ptr, ValueVisitor&& on_value, GetterVisitor&& on_getter)\n";
            os << "    {\n";
            bool usedGetterMut = false;
            for (std::size_t i = 0; i < members.size(); ++i)
            {
                const EmittedMember& m = members[i];
                if (m.isBitField)
                {
                    usedGetterMut = true;
                    const std::string getterName = BuildBitGetterName(m.displayName);
                    os << "        on_getter(\"" << EscapeString(m.displayName) << "\", &::wave::ReflectAccess<" << recType << ">::" << getterName << ", " << m.bitWidth << ");\n";
                }
                else
                {
                    if (m.asBoolStorage)
                    {
                        os << "        on_ptr(\"" << EscapeString(m.displayName) << "\", ::wave::as_bool_storage_ptr(std::addressof(" << m.mutExpr << ")));\n";
                    }
                    else if (m.exprIsPointerAlready)
                    {
                        os << "        on_ptr(\"" << EscapeString(m.displayName) << "\", " << m.mutExpr << ");\n";
                    }
                    else
                    {
                        os << "        on_ptr(\"" << EscapeString(m.displayName) << "\", std::addressof(" << m.mutExpr << "));\n";
                    }
                }
            }
            os << "        (void)on_value;\n";
            if (!usedGetterMut) os << "        (void)on_getter;\n";
            os << "    }\n";
            os << "};\n\n";
        }
        os << "} // namespace wave\n\n";

        os << "namespace reflect {\n\n";
        for (std::size_t oi = 0; oi < emitOrder.size(); ++oi)
        {
            const RecordInfo& rec = *emitOrder[oi];
            const std::string recType = emittedTypeByQName[rec.qualifiedName];
            os << BuildClassSpecializationPrefix(rec);
            os << "struct is_reflected<" << recType << "> : std::true_type {};\n\n";
        }

        for (std::size_t oi = 0; oi < emitOrder.size(); ++oi)
        {
            const RecordInfo& rec = *emitOrder[oi];
            const std::string recType = emittedTypeByQName[rec.qualifiedName];
            os << BuildClassSpecializationPrefix(rec);
            os << "struct reflected_visitor<" << recType << ">\n";
            os << "{\n";
            os << "    template <typename PtrVisitor, typename ValueVisitor, typename GetterVisitor>\n";
            os << "    static void visit(const " << recType << "* obj, PtrVisitor&& on_ptr, ValueVisitor&& on_value, GetterVisitor&& on_getter)\n";
            os << "    {\n";
            os << "        ::wave::ReflectAccess<" << recType << ">::visit(obj, std::forward<PtrVisitor>(on_ptr), std::forward<ValueVisitor>(on_value), std::forward<GetterVisitor>(on_getter));\n";
            os << "    }\n\n";
            os << "    template <typename PtrVisitor, typename ValueVisitor>\n";
            os << "    static void visit(const " << recType << "* obj, PtrVisitor&& on_ptr, ValueVisitor&& on_value)\n";
            os << "    {\n";
            os << "        ::wave::ReflectAccess<" << recType << ">::visit(obj, std::forward<PtrVisitor>(on_ptr), std::forward<ValueVisitor>(on_value));\n";
            os << "    }\n\n";
            os << "    template <typename PtrVisitor, typename ValueVisitor, typename GetterVisitor>\n";
            os << "    static void visit(" << recType << "* obj, PtrVisitor&& on_ptr, ValueVisitor&& on_value, GetterVisitor&& on_getter)\n";
            os << "    {\n";
            os << "        ::wave::ReflectAccess<" << recType << ">::visit(obj, std::forward<PtrVisitor>(on_ptr), std::forward<ValueVisitor>(on_value), std::forward<GetterVisitor>(on_getter));\n";
            os << "    }\n";
            os << "};\n\n";
        }

        os << "#ifdef WAVE_RUNTIME_AVAILABLE\n";
        os << "inline void " << BuildWaveRegistrationFunctionName(opt) << "()\n";
        os << "{\n";
        for (std::size_t oi = 0; oi < emitOrder.size(); ++oi)
        {
            const RecordInfo& rec = *emitOrder[oi];
            if (rec.templateKind == RecordTemplateKind::Primary) continue;
            const std::string recType = emittedTypeByQName[rec.qualifiedName];
            os << "    static wave::DynamicTypeRegistration<" << recType << "> reg_" << oi << ";\n";
            os << "    (void)reg_" << oi << ";\n";
        }
        os << "}\n\n";
        os << "inline void " << BuildConvenienceRegistrationFunctionName(opt) << "()\n";
        os << "{\n";
        os << "    " << BuildWaveRegistrationFunctionName(opt) << "();\n";
        os << "}\n\n";
        os << "namespace generated_registration_detail_" << GetOutputStem(opt) << " {\n";
        os << "struct AutoDynamicRegistration\n";
        os << "{\n";
        os << "    AutoDynamicRegistration() { ::reflect::" << BuildWaveRegistrationFunctionName(opt) << "(); }\n";
        os << "};\n";
        os << "static AutoDynamicRegistration auto_dynamic_registration_instance;\n";
        os << "} // namespace generated_registration_detail_" << GetOutputStem(opt) << "\n";
        os << "#endif\n\n";
        os << "} // namespace reflect\n\n";
        os << "#endif\n";
        return FlushAndCheckFile(os, opt.outputHeader, "[emit generated file write error]");
    }

    bool GenerateOneHeader(const Options& baseOpt,
        const std::string& inputHeader,
        const std::string& outputHeader,
        const std::string& headerInclude,
        std::size_t& recordCount)
    {
        Options opt = baseOpt;
        opt.batchMode = false;
        opt.inputHeader = inputHeader;
        opt.outputHeader = outputHeader;
        opt.headerInclude = headerInclude;

        std::vector<const char*> cargs;
        cargs.reserve(opt.clangArgs.size());
        for (std::size_t i = 0; i < opt.clangArgs.size(); ++i) cargs.push_back(opt.clangArgs[i].c_str());

        CXIndex index = clang_createIndex(0, 0);
        CXTranslationUnit tu = NULL;

        // ReflectGen only needs declarations, fields, bases, template parameters,
        // access specifiers and type spellings. It does not need to semantically
        // validate inline function bodies or constructor bodies from the target
        // simulator headers. Large MSVC/SystemC projects often contain header-only
        // implementations that Clang diagnoses more strictly than MSVC, e.g.
        // array initialization of non-copyable SystemC helper objects. Parsing
        // with SkipFunctionBodies keeps the AST for declarations available while
        // avoiding those implementation-only errors during normal reflection.
        //
        // The diagnostic fallback intentionally does NOT rely on this option: when
        // normal parsing fails before a usable TU is created, it still runs the
        // clang/clang++ syntax probe so the user can see the full compile errors.
        const unsigned parseOptions =
            CXTranslationUnit_DetailedPreprocessingRecord |
            CXTranslationUnit_SkipFunctionBodies |
            CXTranslationUnit_KeepGoing;

        std::cout << "[libclang parse options] DetailedPreprocessingRecord SkipFunctionBodies KeepGoing\n";

        const CXErrorCode err = clang_parseTranslationUnit2(
            index,
            opt.inputHeader.c_str(),
            cargs.empty() ? NULL : &cargs[0],
            static_cast<int>(cargs.size()),
            NULL,
            0,
            parseOptions,
            &tu);

        if (err != CXError_Success || tu == NULL)
        {
            std::cerr << "Failed to parse translation unit: " << opt.inputHeader
                << ". libclang error code = " << static_cast<int>(err)
                << " (" << CxErrorName(err) << ")\n";
            std::cerr << "[libclang input] " << opt.inputHeader << "\n";
            PrintClangArgsForLog(opt.clangArgs);

            if (tu)
            {
                std::cerr << "[diagnostics begin]\n";
                PrintDiagnostics(tu);
                std::cerr << "[diagnostics end]\n";
                clang_disposeTranslationUnit(tu);
            }
            else
            {
                std::cerr << "[diagnostics unavailable] libclang returned NULL TranslationUnit.\n";
            }

            RunLibclangDiagnosticProbe(index, opt);
            RunClangxxDiagnosticProbe(opt);
            clang_disposeIndex(index);
            return false;
        }

        const DiagnosticsSummary diag = PrintDiagnostics(tu);
        if (!DiagnosticsAreAcceptableOrReport(opt, diag))
        {
            clang_disposeTranslationUnit(tu);
            clang_disposeIndex(index);
            return false;
        }

        CollectContext ctx;
        ctx.mainFileOnly = opt.mainFileOnly;
        ctx.tu = tu;

        const CXCursor root = clang_getTranslationUnitCursor(tu);
        clang_visitChildren(root, AstVisitor, &ctx);
        PrintRecordCollectionSummary(opt, ctx);

        const std::string outDir = DirectoryName(opt.outputHeader);
        if (!outDir.empty() && !MakeDirectoryRecursive(outDir))
        {
            PrintDirectoryCreateFailure("[generated header output]", outDir);
            clang_disposeTranslationUnit(tu);
            clang_disposeIndex(index);
            return false;
        }

        std::vector<RecordInfo> emitRecords = ctx.records;
        const std::vector<std::string> requestedRoots = GetRequestedRootClassNames(opt);
        if (!requestedRoots.empty())
        {
            emitRecords = ComputeRootClassClosureRecords(ctx.records, requestedRoots);
        }

        if (!EmitGeneratedFile(emitRecords, opt))
        {
            clang_disposeTranslationUnit(tu);
            clang_disposeIndex(index);
            return false;
        }

        recordCount = emitRecords.size();
        std::cout << "Generated: " << opt.outputHeader << "\n";
        std::cout << "Reflected record count: " << emitRecords.size() << "\n";

        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        return true;
    }

    bool EmitAggregateHeader(const std::string& aggregateHeader,
        const std::vector<std::string>& generatedHeaders)
    {
        const std::string outDir = DirectoryName(aggregateHeader);
        if (!outDir.empty() && !MakeDirectoryRecursive(outDir))
        {
            PrintDirectoryCreateFailure("[aggregate header]", outDir);
            return false;
        }

        errno = 0;
        std::ofstream os(aggregateHeader.c_str(), std::ios::binary);
        if (!os)
        {
            PrintFileOpenFailure("[aggregate header open error]", aggregateHeader);
            return false;
        }

        const std::string guard = MakeHeaderGuard(aggregateHeader);
        os << "#ifndef " << guard << "\n";
        os << "#define " << guard << "\n\n";
        os << "#ifdef new\n";
        os << "#undef new\n";
        os << "#endif\n";
        os << "#ifdef make_shared\n";
        os << "#undef make_shared\n";
        os << "#endif\n";
        os << "#ifdef make_unique\n";
        os << "#undef make_unique\n";
        os << "#endif\n\n";
        os << "#include \"wave_runtime.h\"\n\n";

        for (std::size_t i = 0; i < generatedHeaders.size(); ++i)
        {
            os << "#include \"" << NormalizePathSlashes(MakeAggregateIncludePath(aggregateHeader, generatedHeaders[i])) << "\"\n";
        }

        os << "\n#endif\n";
        return FlushAndCheckFile(os, aggregateHeader, "[aggregate header write error]");
    }

    std::map<std::string, const RecordInfo*> BuildRecordLookupMap(const std::vector<RecordInfo>& records)
    {
        std::map<std::string, const RecordInfo*> byName;
        for (std::size_t i = 0; i < records.size(); ++i)
        {
            byName[CanonicalRecordLookupKey(records[i].qualifiedName)] = &records[i];
            byName[CanonicalRecordLookupKey(records[i].simpleName)] = &records[i];
            if (!records[i].explicitReflectedType.empty()) byName[CanonicalRecordLookupKey(records[i].explicitReflectedType)] = &records[i];
        }
        return byName;
    }

    bool IsBuiltinOrNonRecordLookupKey(const std::string& key)
    {
        if (key.empty()) return true;
        const char* builtins[] = {
            "void", "bool", "char", "signedchar", "unsignedchar", "wchar_t", "char16_t", "char32_t",
            "short", "unsignedshort", "int", "unsignedint", "long", "unsignedlong",
            "longlong", "unsignedlonglong", "float", "double", "longdouble",
            "std::string", "string", "std::nullptr_t", "nullptr_t", "size_t", "std::size_t"
        };
        for (std::size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i)
        {
            if (key == builtins[i]) return true;
        }
        // Non-type template arguments, e.g. std::array<T, 4> or policy constants.
        if (!key.empty() && (std::isdigit(static_cast<unsigned char>(key[0])) || key[0] == '-')) return true;
        if (key.compare(0, 9, "sc_core::") == 0 && !IsWhitelistedSystemCMemberType(key)) return true;
        if (key.compare(0, 8, "reflect::") == 0) return true;
        if (key.compare(0, 6, "wave::") == 0) return true;
        return false;
    }

    bool IsStdClosureStopType(const std::string& key)
    {
        if (key.compare(0, 5, "std::") != 0) return false;
        // These wrappers/containers are understood by wave_runtime; their value
        // type should be part of the reflection closure when it is a business type.
        if (key == "std::array" || key == "std::pair" || key == "std::unique_ptr" ||
            key == "std::shared_ptr" || key == "std::weak_ptr") return false;
        // Most STL containers are blacklisted at runtime; do not recurse into their
        // template arguments, otherwise vector/map internals pull large parts of the
        // project into the generated reflection set.
        return true;
    }

    std::string StripTypeForClosure(std::string s)
    {
        s = Trim(s);
        bool changed = true;
        while (changed)
        {
            changed = false;
            changed = ConsumePrefix(s, "::") || changed;
            changed = ConsumePrefix(s, "class ") || changed;
            changed = ConsumePrefix(s, "struct ") || changed;
            changed = ConsumePrefix(s, "union ") || changed;
            changed = ConsumePrefix(s, "enum ") || changed;
            changed = ConsumePrefix(s, "typename ") || changed;
            changed = ConsumePrefix(s, "const ") || changed;
            changed = ConsumePrefix(s, "volatile ") || changed;
            s = Trim(s);
        }
        // Remove top-level pointer/reference suffixes and array extents.
        int angleDepth = 0;
        std::size_t cut = std::string::npos;
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            const char c = s[i];
            if (c == '<') ++angleDepth;
            else if (c == '>' && angleDepth > 0) --angleDepth;
            else if (angleDepth == 0 && (c == '*' || c == '&' || c == '['))
            {
                cut = i;
                break;
            }
        }
        if (cut != std::string::npos) s = s.substr(0, cut);
        return Trim(s);
    }

    std::string TopLevelTypeKeyForClosure(const std::string& typeName)
    {
        std::string cleaned = StripTypeForClosure(typeName);
        std::size_t lt = std::string::npos;
        int depth = 0;
        for (std::size_t i = 0; i < cleaned.size(); ++i)
        {
            const char c = cleaned[i];
            if (c == '<' && depth == 0) { lt = i; break; }
            if (c == '<') ++depth;
            else if (c == '>' && depth > 0) --depth;
        }
        if (lt != std::string::npos) cleaned = cleaned.substr(0, lt);
        return CanonicalRecordLookupKey(cleaned);
    }

    std::vector<std::string> SplitTopLevelTemplateArgs(const std::string& typeName)
    {
        std::vector<std::string> out;
        const std::size_t lt = typeName.find('<');
        if (lt == std::string::npos) return out;
        int depth = 0;
        std::size_t gt = std::string::npos;
        for (std::size_t i = lt; i < typeName.size(); ++i)
        {
            const char c = typeName[i];
            if (c == '<') ++depth;
            else if (c == '>')
            {
                --depth;
                if (depth == 0) { gt = i; break; }
            }
        }
        if (gt == std::string::npos || gt <= lt + 1) return out;
        const std::string inner = typeName.substr(lt + 1, gt - lt - 1);
        depth = 0;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= inner.size(); ++i)
        {
            const char c = (i < inner.size()) ? inner[i] : ',';
            if (c == '<') ++depth;
            else if (c == '>' && depth > 0) --depth;
            else if (c == ',' && depth == 0)
            {
                std::string arg = Trim(inner.substr(start, i - start));
                if (!arg.empty()) out.push_back(arg);
                start = i + 1;
            }
        }
        return out;
    }

    void AddUniqueClosureCandidate(std::vector<std::string>& out, const std::string& candidate)
    {
        std::string c = Trim(candidate);
        while (ConsumePrefix(c, "::")) c = Trim(c);
        if (c.empty()) return;
        const std::string key = CanonicalRecordLookupKey(c);
        if (key.empty() || IsBuiltinOrNonRecordLookupKey(key) || IsStdClosureStopType(key)) return;
        if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
        if (key != c && std::find(out.begin(), out.end(), key) == out.end()) out.push_back(key);
    }

    void AddClosureTypeCandidatesFromTypeName(const std::string& typeName, std::vector<std::string>& out)
    {
        std::string cleaned = StripTypeForClosure(typeName);
        if (cleaned.empty()) return;
        const std::string topKey = TopLevelTypeKeyForClosure(cleaned);
        if (IsBuiltinOrNonRecordLookupKey(topKey)) return;

        // Full blacklist stop: std::vector/map/list/etc. are intentionally not
        // traversed, including their template arguments, because runtime treats
        // them as blacklisted containers and expanding their payload type tends to
        // pull in huge unrelated object graphs.
        if (IsStdClosureStopType(topKey)) return;

        // Direct business type / business template itself.
        AddUniqueClosureCandidate(out, cleaned);
        AddUniqueClosureCandidate(out, topKey);

        // Recurse into template arguments for business templates and supported
        // wrappers such as std::array/std::pair/smart_ptr/sc_vector/sc_in/vsipIN/vsiiIN.
        const std::vector<std::string> args = SplitTopLevelTemplateArgs(cleaned);
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            AddClosureTypeCandidatesFromTypeName(args[i], out);
        }
    }

    void AddClosureTypeCandidatesFromField(const FieldInfo& field, std::vector<std::string>& out)
    {
        AddClosureTypeCandidatesFromTypeName(field.typeName, out);
        if (!field.canonicalTypeName.empty() && field.canonicalTypeName != field.typeName)
            AddClosureTypeCandidatesFromTypeName(field.canonicalTypeName, out);
        if (!field.declQualifiedName.empty())
            AddClosureTypeCandidatesFromTypeName(field.declQualifiedName, out);
    }

    void AddClosureTypeCandidatesFromBase(const BaseInfo& base, std::vector<std::string>& out)
    {
        AddClosureTypeCandidatesFromTypeName(base.typeName, out);
        if (!base.canonicalTypeName.empty() && base.canonicalTypeName != base.typeName)
            AddClosureTypeCandidatesFromTypeName(base.canonicalTypeName, out);
        if (!base.qualifiedTypeName.empty())
            AddClosureTypeCandidatesFromTypeName(base.qualifiedTypeName, out);
    }

    std::vector<std::string> ReadRootClassNamesFromFile(const Options& opt)
    {
        std::vector<std::string> roots;
        if (opt.rootClassListFile.empty()) return roots;
        std::string path = StripOuterQuotes(ExpandEnvironmentVariablesInPath(opt.rootClassListFile));
        path = NormalizePathSlashes(path);
        if (!IsAbsolutePath(path)) path = MakeAbsoluteLexicalPath(path);
        path = NormalizePathSlashes(CollapseDotDotPath(path));
        const std::string text = ReadWholeFileText(path);
        if (text.empty())
        {
            std::cerr << "[root class list] failed to read or list is empty: " << path << "\n";
            return roots;
        }
        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line))
        {
            const std::size_t hash = line.find('#');
            if (hash != std::string::npos) line.erase(hash);
            const std::size_t slash = line.find("//");
            if (slash != std::string::npos) line.erase(slash);
            line = Trim(line);
            if (!line.empty()) roots.push_back(line);
        }
        std::cout << "[root class list] file=" << path << " roots=" << roots.size() << "\n";
        return roots;
    }

    std::vector<std::string> GetRequestedRootClassNames(const Options& opt)
    {
        std::vector<std::string> roots = opt.rootClassNames;
        const std::vector<std::string> fromFile = ReadRootClassNamesFromFile(opt);
        roots.insert(roots.end(), fromFile.begin(), fromFile.end());
        std::set<std::string> seen;
        std::vector<std::string> dedup;
        for (std::size_t i = 0; i < roots.size(); ++i)
        {
            std::string r = Trim(roots[i]);
            while (ConsumePrefix(r, "::")) r = Trim(r);
            r = CanonicalRecordLookupKey(r);
            if (!r.empty() && seen.insert(r).second) dedup.push_back(r);
        }
        return dedup;
    }

    std::vector<RecordInfo> ComputeRootClassClosureRecords(const std::vector<RecordInfo>& allRecords,
        const std::vector<std::string>& rootNames)
    {
        if (rootNames.empty()) return allRecords;

        const std::map<std::string, const RecordInfo*> byName = BuildRecordLookupMap(allRecords);
        std::map<const RecordInfo*, std::size_t> indexByPtr;
        for (std::size_t i = 0; i < allRecords.size(); ++i) indexByPtr[&allRecords[i]] = i;

        std::set<std::size_t> selected;
        std::vector<const RecordInfo*> queue;
        std::size_t missingRoots = 0;

        auto select_record = [&](const RecordInfo* rec, const std::string& reason) -> bool
        {
            if (!rec) return false;
            std::map<const RecordInfo*, std::size_t>::const_iterator idxIt = indexByPtr.find(rec);
            if (idxIt == indexByPtr.end()) return false;
            if (selected.insert(idxIt->second).second)
            {
                queue.push_back(rec);
                if (gDebugAst)
                {
                    std::cout << "[root closure] add type=" << rec->qualifiedName
                        << " reason=" << reason << "\n";
                }
                return true;
            }
            return false;
        };

        for (std::size_t i = 0; i < rootNames.size(); ++i)
        {
            const RecordInfo* rec = FindRecord(byName, rootNames[i]);
            if (!rec)
            {
                ++missingRoots;
                std::cerr << "[root closure] root class not found in parsed AST: " << rootNames[i] << "\n";
                continue;
            }
            select_record(rec, std::string("root requested=") + rootNames[i]);
        }

        std::size_t closureEdges = 0;
        std::size_t closureMatchedEdges = 0;
        std::size_t closureUnmatchedEdges = 0;
        std::size_t closureSkippedEdges = 0;

        auto try_add_dep_candidates = [&](const RecordInfo& owner,
                                          const std::string& edgeKind,
                                          const std::string& edgeName,
                                          const std::string& rawTypeForLog,
                                          const std::vector<std::string>& candidates)
        {
            ++closureEdges;
            if (candidates.empty())
            {
                ++closureSkippedEdges;
                if (gDebugAst)
                {
                    std::cout << "[root closure skip] owner=" << owner.qualifiedName
                        << " " << edgeKind << "=" << edgeName
                        << " type=" << rawTypeForLog
                        << " reason=no-business-candidate-or-blacklisted\n";
                }
                return;
            }

            bool matched = false;
            for (std::size_t c = 0; c < candidates.size(); ++c)
            {
                const RecordInfo* dep = FindRecord(byName, candidates[c]);
                if (!dep) continue;
                matched = true;
                if (select_record(dep,
                    std::string(edgeKind) + " owner=" + owner.qualifiedName +
                    " name=" + edgeName + " type=" + rawTypeForLog +
                    " candidate=" + candidates[c]))
                {
                    if (gDebugAst)
                    {
                        std::cout << "[root closure] add dep owner=" << owner.qualifiedName
                            << " " << edgeKind << "=" << edgeName
                            << " type=" << rawTypeForLog
                            << " candidate=" << candidates[c]
                            << " -> " << dep->qualifiedName << "\n";
                    }
                }
            }

            if (matched) ++closureMatchedEdges;
            else
            {
                ++closureUnmatchedEdges;
                if (gDebugAst)
                {
                    std::cout << "[root closure miss] owner=" << owner.qualifiedName
                        << " " << edgeKind << "=" << edgeName
                        << " type=" << rawTypeForLog
                        << " candidates=";
                    for (std::size_t c = 0; c < candidates.size(); ++c)
                    {
                        if (c) std::cout << ",";
                        std::cout << candidates[c];
                    }
                    std::cout << " reason=no-recordinfo\n";
                }
            }
        };

        std::size_t cursor = 0;
        while (cursor < queue.size())
        {
            const RecordInfo* rec = queue[cursor++];

            // Direct field types.  This is the main closure edge: root -> member type ->
            // member-of-member type ...  Candidate collection uses original spelling,
            // canonical spelling, and resolved declaration name so typedef/using aliases
            // and pointer/reference wrappers can still pull the real business type in.
            for (std::size_t f = 0; f < rec->fields.size(); ++f)
            {
                const FieldInfo& field = rec->fields[f];
                if (!RecordAllowsAccess(*rec, field.access))
                {
                    if (gDebugAst)
                    {
                        std::cout << "[root closure skip] owner=" << rec->qualifiedName
                            << " field=" << field.name
                            << " type=" << field.typeName
                            << " reason=field-access-not-allowed\n";
                    }
                    continue;
                }
                std::vector<std::string> candidates;
                AddClosureTypeCandidatesFromField(field, candidates);
                if (gDebugAst)
                {
                    std::cout << "[root closure field candidates] owner=" << rec->qualifiedName
                        << " field=" << field.name
                        << " type=" << field.typeName
                        << " canonical=" << field.canonicalTypeName
                        << " decl=" << field.declQualifiedName
                        << " candidates=";
                    for (std::size_t ci = 0; ci < candidates.size(); ++ci)
                    {
                        if (ci) std::cout << ",";
                        std::cout << candidates[ci];
                    }
                    std::cout << "\n";
                }
                try_add_dep_candidates(*rec, "field", field.name, field.typeName, candidates);
            }

            // Base specifier types are dependency edges only.  Generated visitors still
            // do NOT enumerate base-class members, per the no-base-members policy.
            // This handles both direct and multi-level wrappers:
            //   MyPort -> MyBasePort -> vsiiIN<Payload>
            // by selecting MyBasePort first, then processing its bases in a later queue pass.
            // Exact vsipIN/OUT/INOUT port fields still contribute their T through
            // normal template-argument extraction, but inheritance-based .peek()
            // value-source handling is intentionally tied to vsiiIN/OUT/INOUT.
            for (std::size_t b = 0; b < rec->bases.size(); ++b)
            {
                const BaseInfo& base = rec->bases[b];
                if (base.access == CX_CXXPrivate || base.access == CX_CXXProtected)
                {
                    if (gDebugAst)
                    {
                        std::cout << "[root closure skip] owner=" << rec->qualifiedName
                            << " base=" << base.typeName
                            << " reason=base-access-not-public\n";
                    }
                    continue;
                }
                std::vector<std::string> candidates;
                AddClosureTypeCandidatesFromBase(base, candidates);
                if (gDebugAst)
                {
                    std::cout << "[root closure base candidates] owner=" << rec->qualifiedName
                        << " base=" << base.typeName
                        << " canonical=" << base.canonicalTypeName
                        << " qtype=" << base.qualifiedTypeName
                        << " candidates=";
                    for (std::size_t ci = 0; ci < candidates.size(); ++ci)
                    {
                        if (ci) std::cout << ",";
                        std::cout << candidates[ci];
                    }
                    std::cout << "\n";
                }
                try_add_dep_candidates(*rec, "base", base.qualifiedTypeName.empty() ? base.typeName : base.qualifiedTypeName, base.typeName, candidates);
            }
        }

        std::vector<RecordInfo> out;
        out.reserve(selected.size());
        for (std::size_t i = 0; i < allRecords.size(); ++i)
        {
            if (selected.find(i) != selected.end()) out.push_back(allRecords[i]);
        }
        std::cout << "[root closure] requested_roots=" << rootNames.size()
            << " missing_roots=" << missingRoots
            << " all_records=" << allRecords.size()
            << " selected_records=" << out.size()
            << " edges=" << closureEdges
            << " matched_edges=" << closureMatchedEdges
            << " unmatched_edges=" << closureUnmatchedEdges
            << " skipped_edges=" << closureSkippedEdges << "\n";
        return out;
    }

    bool WriteBatchAggregateInputHeader(const std::string& path, const std::vector<std::string>& headers)
    {
        const std::string dir = DirectoryName(path);
        if (!dir.empty() && !MakeDirectoryRecursive(dir))
        {
            PrintDirectoryCreateFailure("[batch temporary aggregate input]", dir);
            return false;
        }

        errno = 0;
        std::ofstream os(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        if (!os)
        {
            PrintFileOpenFailure("[batch temporary aggregate input open error]", path);
            return false;
        }

        os << "// Generated by ReflectGen. Do not edit.\n";
        os << "// This file is a temporary aggregate input used to parse all business headers once.\n\n";
        for (std::size_t i = 0; i < headers.size(); ++i)
        {
            os << "#include \"" << EscapeString(NormalizePathSlashes(headers[i])) << "\"\n";
        }
        return FlushAndCheckFile(os, path, "[batch temporary aggregate input write error]");
    }

    bool CollectRecordsFromTranslationUnit(const Options& opt,
        CXTranslationUnit tu,
        bool mainFileOnly,
        bool useWhitelist,
        const std::set<std::string>& whitelist,
        std::vector<RecordInfo>& records)
    {
        CollectContext ctx;
        ctx.mainFileOnly = mainFileOnly;
        ctx.useFileWhitelist = useWhitelist;
        ctx.sourceFileWhitelist = whitelist;
        ctx.tu = tu;

        const CXCursor root = clang_getTranslationUnitCursor(tu);
        clang_visitChildren(root, AstVisitor, &ctx);
        PrintRecordCollectionSummary(opt, ctx);
        records.swap(ctx.records);
        return true;
    }


    bool IsClangXLanguageSelector(const std::string& arg)
    {
        return arg == "-x" || arg.compare(0, 2, "-x") == 0;
    }

    bool IsClangOutputOrCompileOnlyArg(const std::string& arg)
    {
        return arg == "-o" || arg == "-c" || arg == "-S" ||
            arg == "-E" || arg == "-P" || arg == "-fsyntax-only";
    }

    std::vector<std::string> BuildClangxxPreprocessArgs(const Options& opt, const std::string& expandedPath)
    {
        std::vector<std::string> args;
        args.reserve(opt.clangArgs.size() + 10);

        bool sawX = false;
        for (std::size_t i = 0; i < opt.clangArgs.size(); ++i)
        {
            const std::string& a = opt.clangArgs[i];
            if (a == "-x" && i + 1 < opt.clangArgs.size())
            {
                args.push_back("-x");
                args.push_back("c++");
                ++i;
                sawX = true;
                continue;
            }
            if (a.compare(0, 2, "-x") == 0 && a.size() > 2)
            {
                args.push_back("-xc++");
                sawX = true;
                continue;
            }
            if (a == "-o" && i + 1 < opt.clangArgs.size())
            {
                ++i;
                continue;
            }
            if (IsClangOutputOrCompileOnlyArg(a)) continue;
            args.push_back(a);
        }

        if (!sawX)
        {
            args.insert(args.begin(), "c++");
            args.insert(args.begin(), "-x");
        }

        AddUniqueArg(args, "-E");
        AddUniqueArg(args, "-P");
        AddUniqueArg(args, "-ferror-limit=0");
        args.push_back("-o");
        args.push_back(expandedPath);

        const std::string inputDir = DirectoryName(opt.inputHeader);
        if (!inputDir.empty()) AddUniqueArg(args, "-I" + inputDir);
        return args;
    }

    std::vector<std::string> BuildExpandedParseArgs(const Options& opt)
    {
        std::vector<std::string> args;
        args.push_back("-x");
        args.push_back("c++");

        // The preprocessed .ii is already macro-expanded, so do not re-apply
        // -D/-I/-include arguments.  Keep only language/ABI switches that can
        // affect parsing of the expanded token stream.
        for (std::size_t i = 0; i < opt.clangArgs.size(); ++i)
        {
            const std::string& a = opt.clangArgs[i];
            if (a == "-std" && i + 1 < opt.clangArgs.size())
            {
                args.push_back(a);
                args.push_back(opt.clangArgs[++i]);
                continue;
            }
            if (a.compare(0, 5, "-std=") == 0 ||
                a == "-fms-extensions" ||
                a == "-fms-compatibility" ||
                a == "-fdelayed-template-parsing" ||
                a == "-fdeclspec" ||
                a == "-fno-delayed-template-parsing" ||
                a == "-fchar8_t" ||
                a == "-fno-char8_t")
            {
                AddUniqueArg(args, a);
                continue;
            }
            if ((a == "--target" || a == "-target") && i + 1 < opt.clangArgs.size())
            {
                args.push_back(a);
                args.push_back(opt.clangArgs[++i]);
                continue;
            }
            if ((a.compare(0, 9, "--target=") == 0) || a.compare(0, 7, "-target") == 0)
            {
                AddUniqueArg(args, a);
                continue;
            }
            if (a == "-m64" || a == "-m32") AddUniqueArg(args, a);
        }
        return args;
    }

    std::string ReflectGenTempBaseForExpandedFriendScan(const Options& opt)
    {
        std::string logPath = AbsoluteDiagnosticPathOrEmpty(opt.logFile);
        if (logPath.empty()) logPath = JoinPath(CurrentWorkingDirectory(), "reflectgen.log");
        std::string base = logPath;
        if (base.empty()) base = JoinPath(CurrentWorkingDirectory(), "reflectgen");
        return base + ".expanded_friend";
    }

    bool WriteCommandResponseFile(const std::string& rspPath,
                                  const std::vector<std::string>& args,
                                  const std::string& input)
    {
        const std::string rspDir = DirectoryName(rspPath);
        if (!rspDir.empty() && !MakeDirectoryRecursive(rspDir))
        {
            PrintDirectoryCreateFailure("[expanded friend response file]", rspDir);
            return false;
        }
        errno = 0;
        std::ofstream rsp(rspPath.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        if (!rsp)
        {
            PrintFileOpenFailure("[expanded friend response file open error]", rspPath);
            return false;
        }
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            rsp << ShellQuote(args[i]) << "\n";
        }
        rsp << ShellQuote(input) << "\n";
        return FlushAndCheckFile(rsp, rspPath, "[expanded friend response file write error]");
    }

    bool RunExpandedFriendPreprocess(const Options& opt, std::string& expandedPath)
    {
        expandedPath.clear();
        const std::string clangxx = NormalizePathSlashes(StripOuterQuotes(ExpandEnvironmentVariablesInPath(ResolveClangxxPath(opt))));
        const std::string input = NormalizePathSlashes(MakeAbsoluteLexicalPath(opt.inputHeader));
        const std::string tempBase = ReflectGenTempBaseForExpandedFriendScan(opt);
        const std::string rspPath = tempBase + ".rsp";
        const std::string outPath = tempBase + ".ii";
        const std::string capturePath = tempBase + ".clangxx.out.txt";
        std::vector<std::string> args = BuildClangxxPreprocessArgs(opt, outPath);

        if (gDebugAst)
        {
            std::cerr << "[expanded friend scan] clang++=" << clangxx << "\n";
            std::cerr << "[expanded friend scan] input=" << input << "\n";
            std::cerr << "[expanded friend scan] expanded=" << outPath << "\n";
        }

        if (!WriteCommandResponseFile(rspPath, args, input)) return false;
        const std::string cmd = ShellQuote(clangxx) + " @" + ShellQuote(rspPath) + " 2>&1";
        int rc = -1;
        const std::string captured = CaptureProcessOutput(cmd, rc);
        {
            std::ofstream os(capturePath.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
            if (os) os << captured;
        }
        if (rc != 0 || !FileExistsForDiagnostics(outPath))
        {
            std::cerr << "[expanded friend scan skipped] clang++ -E failed or did not create expanded TU. rc=" << rc
                      << " output=" << capturePath << "\n";
            if (gDebugAst && !captured.empty())
            {
                std::cerr << "========== expanded friend clang++ output begin ==========" << "\n";
                std::cerr << captured;
                if (captured[captured.size() - 1] != '\n') std::cerr << "\n";
                std::cerr << "========== expanded friend clang++ output end ==========" << "\n";
            }
            return false;
        }
        expandedPath = outPath;
        return true;
    }

    void CollectExpandedFriendRecordsFromCursor(CXTranslationUnit tu, CXCursor cursor, std::set<std::string>& out)
    {
        struct Payload
        {
            CXTranslationUnit tu;
            std::set<std::string>* out;
        } payload{ tu, &out };

        clang_visitChildren(
            cursor,
            [](CXCursor child, CXCursor, CXClientData clientData) {
                Payload* p = static_cast<Payload*>(clientData);
                const CXCursorKind kind = clang_getCursorKind(child);
                if ((IsRecordKind(kind) || kind == CXCursor_ClassTemplate) && clang_isCursorDefinition(child))
                {
                    const std::string qn = GetQualifiedName(child);
                    if (!qn.empty() && CursorHasReflectAccessFriend(p->tu, child))
                    {
                        p->out->insert(qn);
                        if (gDebugAst)
                        {
                            PrintCursorDebugLine("[expanded friend kept]", child,
                                "record=[" + qn + "] text=[" + ShortenForDebugLog(GetCursorSourceText(p->tu, child)) + "]");
                        }
                    }
                }
                return CXChildVisit_Recurse;
            },
            &payload);
    }

    bool CollectExpandedReflectFriendRecords(const Options& opt, std::set<std::string>& expandedFriendRecords)
    {
        expandedFriendRecords.clear();
        std::string expandedPath;
        if (!RunExpandedFriendPreprocess(opt, expandedPath)) return false;

        std::vector<std::string> parseArgs = BuildExpandedParseArgs(opt);
        std::vector<const char*> cargs;
        cargs.reserve(parseArgs.size());
        for (std::size_t i = 0; i < parseArgs.size(); ++i) cargs.push_back(parseArgs[i].c_str());

        CXIndex index = clang_createIndex(0, 0);
        CXTranslationUnit tu = NULL;
        const unsigned parseOptions = CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_KeepGoing;
        const CXErrorCode err = clang_parseTranslationUnit2(
            index,
            expandedPath.c_str(),
            cargs.empty() ? NULL : &cargs[0],
            static_cast<int>(cargs.size()),
            NULL,
            0,
            parseOptions,
            &tu);

        if (err != CXError_Success || tu == NULL)
        {
            std::cerr << "[expanded friend scan skipped] failed to parse expanded TU: " << expandedPath
                      << " err=" << static_cast<int>(err) << " (" << CxErrorName(err) << ")\n";
            if (tu)
            {
                std::cerr << "[expanded friend diagnostics begin]\n";
                PrintDiagnostics(tu);
                std::cerr << "[expanded friend diagnostics end]\n";
                clang_disposeTranslationUnit(tu);
            }
            clang_disposeIndex(index);
            return false;
        }

        const DiagnosticsSummary diag = PrintDiagnostics(tu);
        if (!DiagnosticsAreAcceptableOrReport(opt, diag))
        {
            std::cerr << "[expanded friend scan skipped] expanded TU diagnostics are not acceptable.\n";
            clang_disposeTranslationUnit(tu);
            clang_disposeIndex(index);
            return false;
        }

        CollectExpandedFriendRecordsFromCursor(tu, clang_getTranslationUnitCursor(tu), expandedFriendRecords);
        std::cout << "[expanded friend scan] records_with_reflect_friend=" << expandedFriendRecords.size()
                  << " expanded_tu=" << expandedPath << "\n";
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        return true;
    }

    void ApplyExpandedReflectFriendRecords(std::vector<RecordInfo>& records, const std::set<std::string>& expandedFriendRecords)
    {
        if (expandedFriendRecords.empty()) return;
        std::size_t newlyEnabled = 0;
        for (std::size_t i = 0; i < records.size(); ++i)
        {
            RecordInfo& rec = records[i];
            if (rec.qualifiedName.empty()) continue;
            if (expandedFriendRecords.find(rec.qualifiedName) != expandedFriendRecords.end())
            {
                if (!rec.allowPrivateReflect) ++newlyEnabled;
                rec.allowPrivateReflect = true;
                if (gDebugAst)
                {
                    std::cout << "[expanded friend applied] record=" << rec.qualifiedName << " allowPrivateReflect=yes\n";
                }
            }
        }
        std::cout << "[expanded friend scan] matched_original_records=" << newlyEnabled << " newly_enabled\n";
    }

    bool ParseAndCollectRecords(const Options& opt,
        bool mainFileOnly,
        bool useWhitelist,
        const std::set<std::string>& whitelist,
        std::vector<RecordInfo>& records)
    {
        std::vector<const char*> cargs;
        cargs.reserve(opt.clangArgs.size());
        for (std::size_t i = 0; i < opt.clangArgs.size(); ++i) cargs.push_back(opt.clangArgs[i].c_str());

        CXIndex index = clang_createIndex(0, 0);
        CXTranslationUnit tu = NULL;
        const unsigned parseOptions =
            CXTranslationUnit_DetailedPreprocessingRecord |
            CXTranslationUnit_SkipFunctionBodies |
            CXTranslationUnit_KeepGoing;

        std::cout << "[libclang parse options] DetailedPreprocessingRecord SkipFunctionBodies KeepGoing\n";

        const CXErrorCode err = clang_parseTranslationUnit2(
            index,
            opt.inputHeader.c_str(),
            cargs.empty() ? NULL : &cargs[0],
            static_cast<int>(cargs.size()),
            NULL,
            0,
            parseOptions,
            &tu);

        if (err != CXError_Success || tu == NULL)
        {
            std::cerr << "Failed to parse translation unit: " << opt.inputHeader
                << ". libclang error code = " << static_cast<int>(err)
                << " (" << CxErrorName(err) << ")\n";
            std::cerr << "[libclang input] " << opt.inputHeader << "\n";
            PrintClangArgsForLog(opt.clangArgs);

            if (tu)
            {
                std::cerr << "[diagnostics begin]\n";
                PrintDiagnostics(tu);
                std::cerr << "[diagnostics end]\n";
                clang_disposeTranslationUnit(tu);
            }
            else
            {
                std::cerr << "[diagnostics unavailable] libclang returned NULL TranslationUnit.\n";
            }

            RunLibclangDiagnosticProbe(index, opt);
            RunClangxxDiagnosticProbe(opt);
            clang_disposeIndex(index);
            return false;
        }

        const DiagnosticsSummary diag = PrintDiagnostics(tu);
        if (!DiagnosticsAreAcceptableOrReport(opt, diag))
        {
            clang_disposeTranslationUnit(tu);
            clang_disposeIndex(index);
            return false;
        }
        const bool ok = CollectRecordsFromTranslationUnit(opt, tu, mainFileOnly, useWhitelist, whitelist, records);
        if (ok && opt.expandedFriendScan)
        {
            std::set<std::string> expandedFriendRecords;
            if (CollectExpandedReflectFriendRecords(opt, expandedFriendRecords))
            {
                ApplyExpandedReflectFriendRecords(records, expandedFriendRecords);
            }
            else
            {
                std::cerr << "[expanded friend scan warning] fallback scan failed; keeping AST/token friend results only.\n";
            }
        }
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        return ok;
    }

    bool IsExistingRegularFile(const std::string& path)
    {
        if (path.empty()) return false;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) return false;
#ifdef _WIN32
        return (st.st_mode & _S_IFREG) != 0;
#else
        return S_ISREG(st.st_mode);
#endif
    }

    bool HasWildcardChars(const std::string& path)
    {
        return path.find('*') != std::string::npos || path.find('?') != std::string::npos;
    }

    bool ExtractNextXmlStartTag(const std::string& xml, std::size_t& pos, std::string& tagText)
    {
        while (true)
        {
            const std::size_t lt = xml.find('<', pos);
            if (lt == std::string::npos) return false;
            const std::size_t gt = xml.find('>', lt + 1);
            if (gt == std::string::npos) return false;
            pos = gt + 1;
            tagText = xml.substr(lt, gt - lt + 1);
            if (tagText.size() >= 2 && (tagText[1] == '/' || tagText[1] == '!' || tagText[1] == '?'))
            {
                continue;
            }
            return true;
        }
    }

    std::string ResolveProjectHeaderItemPath(std::string item,
        const std::string& projectDir,
        const std::string& config,
        const std::string& platform)
    {
        item = StripOuterQuotes(ExpandMsbuildMacros(item, projectDir, config, platform));
        item = NormalizePathSlashes(item);
        if (item.empty()) return std::string();
        if (!IsAbsolutePath(item)) item = JoinPath(projectDir, item);
        return NormalizePathSlashes(CollapseDotDotPath(item));
    }

    bool IsExternalDependenciesFilterName(std::string filter)
    {
        filter = ToLowerAscii(Trim(DecodeXmlEntities(filter)));
        std::replace(filter.begin(), filter.end(), '\\', '/');
        while (!filter.empty() && filter[0] == '/') filter.erase(filter.begin());
        while (!filter.empty() && filter[filter.size() - 1] == '/') filter.erase(filter.size() - 1);

        // Visual Studio English UI uses "External Dependencies".  Some localized
        // installations write the localized text into the .vcxproj.filters file.
        // Keep both checks here so these automatically discovered dependency
        // headers are never promoted into reflection targets.
        const char* externalDependenciesZhUtf8 = "\xE5\xA4\x96\xE9\x83\xA8\xE4\xBE\x9D\xE8\xB5\x96\xE9\xA1\xB9";
        const std::string externalDependenciesZhPrefix = std::string(externalDependenciesZhUtf8) + "/";

        return filter == "external dependencies" ||
            filter.compare(0, std::strlen("external dependencies/"), "external dependencies/") == 0 ||
            filter == externalDependenciesZhUtf8 ||
            filter.compare(0, externalDependenciesZhPrefix.size(), externalDependenciesZhPrefix) == 0;
    }

    std::string MakeVcxprojFiltersPath(const std::string& vcxprojPath)
    {
        std::string p = NormalizePathSlashes(ExpandEnvironmentVariablesInPath(vcxprojPath));
        const std::string lower = ToLowerAscii(p);
        if (EndsWithString(lower, ".vcxproj.filters")) return p;
        return p + ".filters";
    }

    bool ExtractNextXmlElementBlock(const std::string& xml,
        const std::string& tag,
        std::size_t& pos,
        std::string& startTag,
        std::string& body)
    {
        const std::string open = "<" + tag;
        const std::string close = "</" + tag + ">";
        while (true)
        {
            const std::size_t lt = xml.find(open, pos);
            if (lt == std::string::npos) return false;

            const std::size_t afterOpen = lt + open.size();
            if (afterOpen < xml.size())
            {
                const char c = xml[afterOpen];
                if (!(std::isspace(static_cast<unsigned char>(c)) || c == '>' || c == '/'))
                {
                    pos = afterOpen;
                    continue;
                }
            }

            const std::size_t gt = xml.find('>', afterOpen);
            if (gt == std::string::npos) return false;

            startTag = xml.substr(lt, gt - lt + 1);
            body.clear();

            // Self-closing item: <ClInclude Include="x.h" />
            std::size_t lastNonSpace = gt;
            while (lastNonSpace > lt && std::isspace(static_cast<unsigned char>(xml[lastNonSpace - 1]))) --lastNonSpace;
            if (lastNonSpace > lt && xml[lastNonSpace - 1] == '/')
            {
                pos = gt + 1;
                return true;
            }

            const std::size_t end = xml.find(close, gt + 1);
            if (end == std::string::npos) return false;
            body = xml.substr(gt + 1, end - gt - 1);
            pos = end + close.size();
            return true;
        }
    }

    std::string ExtractFirstXmlTagValueTrimmed(const std::string& block, const std::string& tag)
    {
        const std::vector<std::string> values = ExtractXmlTagValues(block, tag);
        if (values.empty()) return std::string();
        return Trim(values.front());
    }

    void CollectHeaderFilesFromVcxproj(const Options& opt,
        std::vector<std::string>& outHeaders,
        std::string& projectDirOut)
    {
        outHeaders.clear();
        projectDirOut.clear();
        if (opt.vcxprojPath.empty()) return;

        const std::string vcxproj = NormalizePathSlashes(ExpandEnvironmentVariablesInPath(opt.vcxprojPath));
        const std::string filtersPath = MakeVcxprojFiltersPath(vcxproj);
        const std::string xml = ReadWholeFileText(filtersPath);
        if (xml.empty())
        {
            std::cerr << "[vcxproj filters] failed to read filters file: " << filtersPath << "\n";
            std::cerr << "[vcxproj filters] no project headers are added from Visual Studio filters. "
                << "Batch roots/dir-list are still used.\n";
            return;
        }

        std::string projectDir = DirectoryName(vcxproj);
        if (EndsWithString(ToLowerAscii(vcxproj), ".vcxproj.filters"))
        {
            projectDir = DirectoryName(vcxproj);
        }
        projectDirOut = projectDir;

        std::set<std::string> seen;
        std::size_t clIncludeItems = 0;
        std::size_t headerCandidates = 0;
        std::size_t skippedCondition = 0;
        std::size_t skippedExternalDependencies = 0;
        std::size_t skippedUnresolved = 0;
        std::size_t skippedWildcard = 0;
        std::size_t skippedMissing = 0;
        std::size_t skippedSystem = 0;
        std::size_t rootFilterItems = 0;

        std::size_t pos = 0;
        std::string startTag;
        std::string body;
        while (ExtractNextXmlElementBlock(xml, "ClInclude", pos, startTag, body))
        {
            if (!ConditionMatchesConfigPlatform(startTag, opt.configuration, opt.platform))
            {
                ++skippedCondition;
                continue;
            }

            const std::string includeAttr = ExtractXmlAttribute(startTag, "Include");
            if (includeAttr.empty()) continue;
            ++clIncludeItems;

            const std::string filterName = ExtractFirstXmlTagValueTrimmed(body, "Filter");
            if (filterName.empty()) ++rootFilterItems;
            if (IsExternalDependenciesFilterName(filterName))
            {
                ++skippedExternalDependencies;
                if (gDebugAst) std::cerr << "[vcxproj filters] skip External Dependencies header: " << includeAttr << " filter=[" << filterName << "]\n";
                continue;
            }

            const std::vector<std::string> parts = SplitSemicolonList(includeAttr);
            for (std::size_t i = 0; i < parts.size(); ++i)
            {
                std::string item = StripOuterQuotes(Trim(parts[i]));
                if (item.empty()) continue;
                std::string expandedForExt = NormalizePathSlashes(ExpandMsbuildMacros(item, projectDir, opt.configuration, opt.platform));
                expandedForExt = StripOuterQuotes(expandedForExt);
                if (!HasHeaderExtension(expandedForExt)) continue;
                ++headerCandidates;

                if (ContainsAnyUnexpandedVariable(expandedForExt))
                {
                    ++skippedUnresolved;
                    if (gDebugAst) std::cerr << "[vcxproj filters] skip unresolved header item: " << item << " -> " << expandedForExt << "\n";
                    continue;
                }
                if (HasWildcardChars(expandedForExt))
                {
                    ++skippedWildcard;
                    std::cerr << "[vcxproj filters] skip wildcard header item: " << expandedForExt << "\n";
                    continue;
                }

                const std::string full = ResolveProjectHeaderItemPath(item, projectDir, opt.configuration, opt.platform);
                if (!IsExistingRegularFile(full))
                {
                    ++skippedMissing;
                    if (gDebugAst) std::cerr << "[vcxproj filters] skip missing header item: " << full << "\n";
                    continue;
                }
                if (IsReflectionSystemHeaderCandidate(full))
                {
                    ++skippedSystem;
                    if (gDebugAst) std::cerr << "[vcxproj filters] skip reflection-system header: " << full << "\n";
                    continue;
                }

                const std::string key = CanonicalSourcePathKey(full);
                if (seen.insert(key).second)
                {
                    outHeaders.push_back(full);
                }
            }
        }

        std::cout << "[vcxproj filters] filters=" << filtersPath
            << " clIncludeItems=" << clIncludeItems
            << " headerCandidates=" << headerCandidates
            << " added=" << outHeaders.size()
            << " rootFilterItems=" << rootFilterItems
            << " skippedCondition=" << skippedCondition
            << " skippedExternalDependencies=" << skippedExternalDependencies
            << " skippedUnresolved=" << skippedUnresolved
            << " skippedWildcard=" << skippedWildcard
            << " skippedMissing=" << skippedMissing
            << " skippedSystem=" << skippedSystem
            << "\n";
    }

    std::vector<std::string> ExtractIncludeDirsFromClangArgs(const Options& opt, const std::string& baseDir)
    {
        std::vector<std::string> dirs;
        std::set<std::string> seen;
        for (std::size_t i = 0; i < opt.clangArgs.size(); ++i)
        {
            std::string dir;
            const std::string& a = opt.clangArgs[i];
            if ((a == "-I" || a == "/I") && i + 1 < opt.clangArgs.size())
            {
                dir = opt.clangArgs[++i];
            }
            else if (a.compare(0, 2, "-I") == 0 && a.size() > 2)
            {
                dir = a.substr(2);
            }
            else if (a.compare(0, 2, "/I") == 0 && a.size() > 2)
            {
                dir = a.substr(2);
            }
            else
            {
                continue;
            }

            dir = StripOuterQuotes(ExpandMsbuildMacros(ExpandEnvironmentVariablesInPath(dir), baseDir, opt.configuration, opt.platform));
            dir = NormalizePathSlashes(dir);
            if (dir.empty() || ContainsAnyUnexpandedVariable(dir)) continue;
            if (!IsAbsolutePath(dir)) dir = JoinPath(baseDir, dir);
            dir = NormalizePathSlashes(CollapseDotDotPath(dir));
            const std::string key = CanonicalSourcePathKey(dir);
            if (seen.insert(key).second) dirs.push_back(dir);
        }
        return dirs;
    }

    std::string StripCxxCommentsForIncludeParse(const std::string& line, bool& inBlockComment)
    {
        std::string out;
        out.reserve(line.size());
        for (std::size_t i = 0; i < line.size(); ++i)
        {
            if (inBlockComment)
            {
                if (i + 1 < line.size() && line[i] == '*' && line[i + 1] == '/')
                {
                    inBlockComment = false;
                    ++i;
                }
                continue;
            }
            if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*')
            {
                inBlockComment = true;
                ++i;
                continue;
            }
            if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')
            {
                break;
            }
            out += line[i];
        }
        return out;
    }

    enum class IncludeDirectiveKind
    {
        None,
        Quoted,
        Angle
    };

    IncludeDirectiveKind ExtractIncludeDirectiveTarget(const std::string& rawLine, std::string& target)
    {
        target.clear();
        std::size_t p = 0;
        while (p < rawLine.size() && std::isspace(static_cast<unsigned char>(rawLine[p]))) ++p;
        if (p >= rawLine.size() || rawLine[p] != '#') return IncludeDirectiveKind::None;
        ++p;
        while (p < rawLine.size() && std::isspace(static_cast<unsigned char>(rawLine[p]))) ++p;
        const std::string kw = "include";
        if (rawLine.compare(p, kw.size(), kw) != 0) return IncludeDirectiveKind::None;
        p += kw.size();
        if (p < rawLine.size() && (std::isalnum(static_cast<unsigned char>(rawLine[p])) || rawLine[p] == '_')) return IncludeDirectiveKind::None;
        while (p < rawLine.size() && std::isspace(static_cast<unsigned char>(rawLine[p]))) ++p;
        if (p >= rawLine.size()) return IncludeDirectiveKind::None;

        const char open = rawLine[p];
        if (open == '"')
        {
            const std::size_t end = rawLine.find('"', p + 1);
            if (end == std::string::npos) return IncludeDirectiveKind::None;
            target = rawLine.substr(p + 1, end - p - 1);
            return IncludeDirectiveKind::Quoted;
        }
        if (open == '<')
        {
            const std::size_t end = rawLine.find('>', p + 1);
            if (end == std::string::npos) return IncludeDirectiveKind::None;
            target = rawLine.substr(p + 1, end - p - 1);
            return IncludeDirectiveKind::Angle;
        }
        return IncludeDirectiveKind::None;
    }

    std::string ResolveQuotedIncludeFromHeader(const std::string& includeText,
        const std::string& headerDir,
        const std::vector<std::string>& includeDirs,
        const Options& opt)
    {
        std::string item = StripOuterQuotes(Trim(includeText));
        item = NormalizePathSlashes(ExpandMsbuildMacros(ExpandEnvironmentVariablesInPath(item), headerDir, opt.configuration, opt.platform));
        if (item.empty() || ContainsAnyUnexpandedVariable(item) || HasWildcardChars(item)) return std::string();

        std::vector<std::string> candidates;
        if (IsAbsolutePath(item))
        {
            candidates.push_back(item);
        }
        else
        {
            candidates.push_back(JoinPath(headerDir, item));
            for (std::size_t i = 0; i < includeDirs.size(); ++i)
            {
                candidates.push_back(JoinPath(includeDirs[i], item));
            }
        }

        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            const std::string full = NormalizePathSlashes(CollapseDotDotPath(candidates[i]));
            if (IsExistingRegularFile(full)) return full;
        }
        return std::string();
    }

    std::string ResolveExistingFileOrDirectoryFromListEntry(const std::string& entryText,
        const std::string& listDir,
        const std::vector<std::string>& includeDirs,
        const Options& opt)
    {
        std::string item = StripOuterQuotes(Trim(entryText));
        item = NormalizePathSlashes(ExpandMsbuildMacros(ExpandEnvironmentVariablesInPath(item), listDir, opt.configuration, opt.platform));
        if (item.empty() || ContainsAnyUnexpandedVariable(item) || HasWildcardChars(item)) return std::string();

        std::vector<std::string> candidates;
        if (IsAbsolutePath(item))
        {
            candidates.push_back(item);
        }
        else
        {
            candidates.push_back(JoinPath(listDir, item));
            for (std::size_t i = 0; i < includeDirs.size(); ++i)
            {
                candidates.push_back(JoinPath(includeDirs[i], item));
            }
        }

        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            const std::string full = NormalizePathSlashes(CollapseDotDotPath(candidates[i]));
            if (IsExistingRegularFile(full) || DirectoryExistsForDiagnostics(full)) return full;
        }
        return std::string();
    }

    struct ActiveIncludeWhitelistState
    {
        const Options* opt = NULL;
        std::string header;
        std::string headerKey;
        std::string headerDir;
        std::vector<std::string> headerLines;
        std::vector<std::string> includeDirs;
        std::set<std::string> seen;
        std::vector<std::string>* outHeaders = NULL;

        std::size_t activeIncludeDirectives = 0;
        std::size_t directIncludeLines = 0;
        std::size_t quotedIncludes = 0;
        std::size_t angleIncludes = 0;
        std::size_t headerCandidates = 0;
        std::size_t skippedNonHeader = 0;
        std::size_t skippedUnresolved = 0;
        std::size_t skippedMissing = 0;
        std::size_t skippedSystem = 0;
        std::size_t skippedDuplicate = 0;
        std::size_t skippedUnparsedLine = 0;
    };

    std::vector<std::string> SplitLinesPreserveEmpty(const std::string& text)
    {
        std::vector<std::string> lines;
        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line)) lines.push_back(line);
        if (!text.empty() && (text[text.size() - 1] == '\n' || text[text.size() - 1] == '\r'))
        {
            // getline already represented the trailing line boundary sufficiently for
            // clang's 1-based line numbers; no synthetic extra line is needed.
        }
        return lines;
    }

    CXChildVisitResult ActiveIncludeWhitelistVisitor(CXCursor cursor, CXCursor, CXClientData clientData)
    {
        ActiveIncludeWhitelistState* st = static_cast<ActiveIncludeWhitelistState*>(clientData);
        if (!st || clang_getCursorKind(cursor) != CXCursor_InclusionDirective) return CXChildVisit_Recurse;

        CXSourceLocation loc = clang_getCursorLocation(cursor);
        CXFile includingFile = NULL;
        unsigned line = 0;
        unsigned column = 0;
        unsigned offset = 0;
        clang_getSpellingLocation(loc, &includingFile, &line, &column, &offset);
        if (!includingFile)
        {
            clang_getExpansionLocation(loc, &includingFile, &line, &column, &offset);
        }
        if (!includingFile) return CXChildVisit_Continue;

        const std::string includingKey = CanonicalSourcePathKey(ToStdString(clang_getFileName(includingFile)));
        if (includingKey != st->headerKey) return CXChildVisit_Continue;

        ++st->activeIncludeDirectives;
        ++st->directIncludeLines;

        std::string includeTarget;
        IncludeDirectiveKind kind = IncludeDirectiveKind::None;
        if (line >= 1 && static_cast<std::size_t>(line) <= st->headerLines.size())
        {
            bool inBlockComment = false;
            const std::string cleaned = StripCxxCommentsForIncludeParse(st->headerLines[static_cast<std::size_t>(line) - 1], inBlockComment);
            kind = ExtractIncludeDirectiveTarget(cleaned, includeTarget);
        }

        if (kind == IncludeDirectiveKind::None)
        {
            // This should be rare.  Do not guess quotation style from clang's resolved
            // file name because that would reintroduce angle/system headers into the
            // whitelist.  The include remains a parse dependency, but not an emit target.
            ++st->skippedUnparsedLine;
            if (gDebugAst) std::cerr << "[include whitelist] skip active include with unparsed source line at " << st->header << ":" << line << "\n";
            return CXChildVisit_Continue;
        }

        if (kind == IncludeDirectiveKind::Angle)
        {
            ++st->angleIncludes;
            return CXChildVisit_Continue;
        }

        ++st->quotedIncludes;
        if (!HasHOrHppExtension(includeTarget))
        {
            ++st->skippedNonHeader;
            return CXChildVisit_Continue;
        }
        ++st->headerCandidates;

        if (ContainsAnyUnexpandedVariable(includeTarget) || HasWildcardChars(includeTarget))
        {
            ++st->skippedUnresolved;
            if (gDebugAst) std::cerr << "[include whitelist] skip unresolved active include: " << includeTarget << "\n";
            return CXChildVisit_Continue;
        }

        std::string full;
        CXFile includedFile = clang_getIncludedFile(cursor);
        if (includedFile)
        {
            full = NormalizePathSlashes(CollapseDotDotPath(ToStdString(clang_getFileName(includedFile))));
        }
        if (full.empty() || !IsExistingRegularFile(full))
        {
            full = ResolveQuotedIncludeFromHeader(includeTarget, st->headerDir, st->includeDirs, *st->opt);
        }
        if (full.empty() || !IsExistingRegularFile(full))
        {
            ++st->skippedMissing;
            if (gDebugAst) std::cerr << "[include whitelist] skip missing active quoted include: " << includeTarget << "\n";
            return CXChildVisit_Continue;
        }

        if (IsReflectionSystemHeaderCandidate(full))
        {
            ++st->skippedSystem;
            if (gDebugAst) std::cerr << "[include whitelist] skip reflection-system header: " << full << "\n";
            return CXChildVisit_Continue;
        }

        const std::string key = CanonicalSourcePathKey(full);
        if (st->seen.insert(key).second)
        {
            st->outHeaders->push_back(NormalizePathSlashes(CollapseDotDotPath(full)));
        }
        else
        {
            ++st->skippedDuplicate;
        }
        return CXChildVisit_Continue;
    }

    void CollectHeaderFilesFromIncludeWhitelistHeader(const Options& opt,
        std::vector<std::string>& outHeaders,
        std::string& rootOut,
        std::string& parseEntryHeaderOut)
    {
        outHeaders.clear();
        rootOut.clear();
        parseEntryHeaderOut.clear();
        if (opt.includeWhitelistHeader.empty()) return;

        std::string header = StripOuterQuotes(ExpandEnvironmentVariablesInPath(opt.includeWhitelistHeader));
        header = NormalizePathSlashes(header);
        if (!IsAbsolutePath(header)) header = MakeAbsoluteLexicalPath(header);
        header = NormalizePathSlashes(CollapseDotDotPath(header));
        if (!IsExistingRegularFile(header))
        {
            std::cerr << "[include whitelist] failed to find whitelist header: " << header << "\n";
            return;
        }

        const std::string text = ReadWholeFileText(header);
        if (text.empty())
        {
            std::cerr << "[include whitelist] failed to read whitelist header or header is empty: " << header << "\n";
            return;
        }

        const std::string headerDir = DirectoryName(header);
        rootOut = headerDir;
        parseEntryHeaderOut = header;

        ActiveIncludeWhitelistState st;
        st.opt = &opt;
        st.header = header;
        st.headerKey = CanonicalSourcePathKey(header);
        st.headerDir = headerDir;
        st.headerLines = SplitLinesPreserveEmpty(text);
        st.includeDirs = ExtractIncludeDirsFromClangArgs(opt, headerDir);
        st.outHeaders = &outHeaders;

        std::vector<const char*> cargs;
        cargs.reserve(opt.clangArgs.size());
        for (std::size_t i = 0; i < opt.clangArgs.size(); ++i) cargs.push_back(opt.clangArgs[i].c_str());

        CXIndex index = clang_createIndex(0, 0);
        CXTranslationUnit tu = NULL;
        const unsigned parseOptions =
            CXTranslationUnit_DetailedPreprocessingRecord |
            CXTranslationUnit_SkipFunctionBodies |
            CXTranslationUnit_KeepGoing;

        std::cout << "[include whitelist] collecting active quoted includes through libclang preprocessing\n";
        const CXErrorCode err = clang_parseTranslationUnit2(
            index,
            header.c_str(),
            cargs.empty() ? NULL : &cargs[0],
            static_cast<int>(cargs.size()),
            NULL,
            0,
            parseOptions,
            &tu);

        if (err != CXError_Success || tu == NULL)
        {
            std::cerr << "[include whitelist] failed to parse whitelist header for active include collection: " << header
                << ". libclang error code = " << static_cast<int>(err)
                << " (" << CxErrorName(err) << ")\n";
            if (tu)
            {
                PrintDiagnostics(tu);
                clang_disposeTranslationUnit(tu);
            }
            clang_disposeIndex(index);
            return;
        }

        const DiagnosticsSummary diag = PrintDiagnostics(tu);
        if (!DiagnosticsAreAcceptableOrReport(opt, diag))
        {
            clang_disposeTranslationUnit(tu);
            clang_disposeIndex(index);
            return;
        }

        const CXCursor root = clang_getTranslationUnitCursor(tu);
        clang_visitChildren(root, ActiveIncludeWhitelistVisitor, &st);
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);

        std::cout << "[include whitelist] header=" << header
            << " mode=active-preprocessed"
            << " activeIncludeDirectives=" << st.activeIncludeDirectives
            << " directIncludeLines=" << st.directIncludeLines
            << " quotedIncludes=" << st.quotedIncludes
            << " angleIncludesIgnored=" << st.angleIncludes
            << " headerCandidates=" << st.headerCandidates
            << " added=" << outHeaders.size()
            << " skippedNonHeader=" << st.skippedNonHeader
            << " skippedUnresolved=" << st.skippedUnresolved
            << " skippedMissing=" << st.skippedMissing
            << " skippedSystem=" << st.skippedSystem
            << " skippedDuplicate=" << st.skippedDuplicate
            << " skippedUnparsedLine=" << st.skippedUnparsedLine
            << " includeSearchDirs=" << st.includeDirs.size()
            << "\n";
    }


    bool LooksLikePlainTargetListEntry(const std::string& line)
    {
        std::string s = StripOuterQuotes(Trim(line));
        if (s.empty()) return false;
        if (s[0] == '#') return false;
        if (s.size() >= 2 && s[0] == '<' && s[s.size() - 1] == '>') return false;
        return true;
    }

    void CollectHeaderFilesFromExtraTargetListFile(const Options& opt,
        std::vector<std::string>& outHeaders,
        std::string& rootOut)
    {
        outHeaders.clear();
        rootOut.clear();
        if (opt.extraTargetListFile.empty()) return;

        std::string listPath = StripOuterQuotes(ExpandEnvironmentVariablesInPath(opt.extraTargetListFile));
        listPath = NormalizePathSlashes(listPath);
        if (!IsAbsolutePath(listPath)) listPath = MakeAbsoluteLexicalPath(listPath);
        listPath = NormalizePathSlashes(CollapseDotDotPath(listPath));
        if (!IsExistingRegularFile(listPath))
        {
            std::cerr << "[extra targets] failed to find target list file: " << listPath << "\n";
            return;
        }

        const std::string text = ReadWholeFileText(listPath);
        if (text.empty())
        {
            std::cerr << "[extra targets] failed to read target list or list is empty: " << listPath << "\n";
            return;
        }

        const std::string listDir = DirectoryName(listPath);
        rootOut = listDir;
        const std::vector<std::string> includeDirs = ExtractIncludeDirsFromClangArgs(opt, listDir);

        std::set<std::string> seen;
        std::size_t lines = 0;
        std::size_t includeDirectives = 0;
        std::size_t plainEntries = 0;
        std::size_t angleIncludesIgnored = 0;
        std::size_t targetCandidates = 0;
        std::size_t fileCandidates = 0;
        std::size_t directoryCandidates = 0;
        std::size_t headersFromDirectories = 0;
        std::size_t skippedNonHeader = 0;
        std::size_t skippedUnresolved = 0;
        std::size_t skippedMissing = 0;
        std::size_t skippedEmptyDirectory = 0;
        std::size_t skippedSystem = 0;
        std::size_t skippedDuplicate = 0;

        std::istringstream iss(text);
        std::string rawLine;
        bool inBlockComment = false;
        while (std::getline(iss, rawLine))
        {
            std::string cleaned = StripCxxCommentsForIncludeParse(rawLine, inBlockComment);
            cleaned = Trim(cleaned);
            if (cleaned.empty()) continue;
            ++lines;

            std::string target;
            const IncludeDirectiveKind kind = ExtractIncludeDirectiveTarget(cleaned, target);
            if (kind == IncludeDirectiveKind::Angle)
            {
                ++includeDirectives;
                ++angleIncludesIgnored;
                continue;
            }
            if (kind == IncludeDirectiveKind::Quoted)
            {
                ++includeDirectives;
            }
            else if (LooksLikePlainTargetListEntry(cleaned))
            {
                target = StripOuterQuotes(cleaned);
                ++plainEntries;
            }
            else
            {
                ++skippedNonHeader;
                continue;
            }

            if (ContainsAnyUnexpandedVariable(target) || HasWildcardChars(target))
            {
                ++skippedUnresolved;
                if (gDebugAst) std::cerr << "[extra targets] skip unresolved target: " << target << "\n";
                continue;
            }
            ++targetCandidates;

            const std::string full = ResolveExistingFileOrDirectoryFromListEntry(target, listDir, includeDirs, opt);
            if (full.empty())
            {
                ++skippedMissing;
                if (gDebugAst) std::cerr << "[extra targets] skip missing target: " << target << "\n";
                continue;
            }

            if (DirectoryExistsForDiagnostics(full))
            {
                ++directoryCandidates;
                std::vector<std::string> dirHeaders;
                // Directory entries in reflect_targets.txt are intentionally non-recursive:
                // only headers directly under this directory are added.
                CollectHeaderFilesInDirectory(full, false, true, dirHeaders);
                std::sort(dirHeaders.begin(), dirHeaders.end());
                if (dirHeaders.empty())
                {
                    ++skippedEmptyDirectory;
                    if (gDebugAst) std::cerr << "[extra targets] directory contains no h/hpp headers: " << full << "\n";
                    continue;
                }

                for (std::size_t i = 0; i < dirHeaders.size(); ++i)
                {
                    const std::string header = NormalizePathSlashes(CollapseDotDotPath(dirHeaders[i]));
                    if (IsReflectionSystemHeaderCandidate(header))
                    {
                        ++skippedSystem;
                        if (gDebugAst) std::cerr << "[extra targets] skip reflection-system header from directory: " << header << "\n";
                        continue;
                    }
                    const std::string key = CanonicalSourcePathKey(header);
                    if (seen.insert(key).second)
                    {
                        outHeaders.push_back(header);
                        ++headersFromDirectories;
                    }
                    else
                    {
                        ++skippedDuplicate;
                    }
                }
                continue;
            }

            if (!HasHOrHppExtension(full))
            {
                ++skippedNonHeader;
                if (gDebugAst) std::cerr << "[extra targets] skip non-h/hpp file: " << full << "\n";
                continue;
            }
            ++fileCandidates;

            if (IsReflectionSystemHeaderCandidate(full))
            {
                ++skippedSystem;
                if (gDebugAst) std::cerr << "[extra targets] skip reflection-system header: " << full << "\n";
                continue;
            }

            const std::string key = CanonicalSourcePathKey(full);
            if (seen.insert(key).second)
            {
                outHeaders.push_back(full);
            }
            else
            {
                ++skippedDuplicate;
            }
        }

        std::cout << "[extra targets] list=" << listPath
            << " lines=" << lines
            << " includeDirectives=" << includeDirectives
            << " plainEntries=" << plainEntries
            << " angleIncludesIgnored=" << angleIncludesIgnored
            << " targetCandidates=" << targetCandidates
            << " fileCandidates=" << fileCandidates
            << " directoryCandidates=" << directoryCandidates
            << " headersFromDirectoriesCurrentLevel=" << headersFromDirectories
            << " added=" << outHeaders.size()
            << " skippedNonHeader=" << skippedNonHeader
            << " skippedUnresolved=" << skippedUnresolved
            << " skippedMissing=" << skippedMissing
            << " skippedEmptyDirectory=" << skippedEmptyDirectory
            << " skippedSystem=" << skippedSystem
            << " skippedDuplicate=" << skippedDuplicate
            << " includeSearchDirs=" << includeDirs.size()
            << "\n";
    }

    struct BatchHeaderJob
    {
        // Canonical source key used for whitelist/grouping.
        std::string input;
        // Original/resolved path spelling used in #include lines. Keep this
        // separate from input because input is lower-cased on Windows for
        // deduplication, while emitting lower-cased paths causes noisy
        // -Wnonportable-include-path warnings and may break #pragma once.
        std::string includePath;
        std::string root;
        std::size_t rootIndex;
    };

    bool RunBatchMode(const Options& opt)
    {
        std::vector<std::string> roots = opt.batchDirs;
        if (opt.batchFromDirListFile)
        {
            if (!ReadBatchDirListFile(opt.batchDirListFile, roots))
            {
                std::cerr << "Failed to read batch directory list file: " << opt.batchDirListFile << "\n";
                return false;
            }
        }

        for (std::size_t i = 0; i < roots.size(); ++i)
        {
            roots[i] = NormalizePathSlashes(ExpandEnvironmentVariablesInPath(roots[i]));
        }
        std::sort(roots.begin(), roots.end());
        roots.erase(std::unique(roots.begin(), roots.end()), roots.end());

        std::vector<std::string> discoveredHeaders;
        std::string discoveredHeaderRoot;
        std::string parseEntryHeader;
        if (!opt.includeWhitelistHeader.empty())
        {
            CollectHeaderFilesFromIncludeWhitelistHeader(opt, discoveredHeaders, discoveredHeaderRoot, parseEntryHeader);
        }
        else if (opt.extraTargetListFile.empty())
        {
            CollectHeaderFilesFromVcxproj(opt, discoveredHeaders, discoveredHeaderRoot);
        }

        std::set<std::string> headersBeforeExtra;
        for (std::size_t i = 0; i < discoveredHeaders.size(); ++i)
        {
            headersBeforeExtra.insert(CanonicalSourcePathKey(discoveredHeaders[i]));
        }

        std::vector<std::string> extraTargetHeaders;
        std::string extraTargetRoot;
        std::vector<std::string> extraParseHeaders;
        std::map<std::string, std::string> extraRootByHeaderKey;
        if (!opt.extraTargetListFile.empty())
        {
            CollectHeaderFilesFromExtraTargetListFile(opt, extraTargetHeaders, extraTargetRoot);
            for (std::size_t i = 0; i < extraTargetHeaders.size(); ++i)
            {
                const std::string key = CanonicalSourcePathKey(extraTargetHeaders[i]);
                extraRootByHeaderKey[key] = extraTargetRoot.empty() ? DirectoryName(extraTargetHeaders[i]) : extraTargetRoot;
                if (headersBeforeExtra.find(key) == headersBeforeExtra.end())
                {
                    extraParseHeaders.push_back(extraTargetHeaders[i]);
                }
                discoveredHeaders.push_back(extraTargetHeaders[i]);
            }
        }

        std::vector<std::string> clearSafetyRoots = roots;
        if (!discoveredHeaderRoot.empty()) clearSafetyRoots.push_back(discoveredHeaderRoot);
        if (!extraTargetRoot.empty()) clearSafetyRoots.push_back(extraTargetRoot);
        if (!parseEntryHeader.empty()) clearSafetyRoots.push_back(DirectoryName(parseEntryHeader));
        std::sort(clearSafetyRoots.begin(), clearSafetyRoots.end());
        clearSafetyRoots.erase(std::unique(clearSafetyRoots.begin(), clearSafetyRoots.end()), clearSafetyRoots.end());

        std::vector<BatchHeaderJob> headers;
        for (std::size_t i = 0; i < roots.size(); ++i)
        {
            std::vector<std::string> found;
            const std::string root = NormalizePathSlashes(roots[i]);
            if (HasHeaderExtension(root) && IsExistingRegularFile(root))
            {
                found.push_back(root);
            }
            else
            {
                // Always collect the supported C/C++ header extensions in the target
                // roots.  The old h/hpp-only directory-list behavior is intentionally
                // relaxed here because the whitelist, not main-file-only, controls what
                // can be reflected after parsing the temporary aggregate header.
                CollectHeaderFilesInDirectory(root, opt.batchRecursive, false, found);
            }

            std::sort(found.begin(), found.end());
            for (std::size_t j = 0; j < found.size(); ++j)
            {
                if (IsReflectionSystemHeaderCandidate(found[j]))
                {
                    if (gDebugAst) std::cout << "[Batch] skip reflection-system header: " << NormalizePathSlashes(found[j]) << "\n";
                    continue;
                }
                BatchHeaderJob job;
                job.input = CanonicalSourcePathKey(found[j]);
                job.includePath = NormalizePathSlashes(CollapseDotDotPath(found[j]));
                job.root = root;
                job.rootIndex = i;
                headers.push_back(job);
            }
        }

        const std::size_t discoveredRootIndex = roots.size();
        for (std::size_t i = 0; i < discoveredHeaders.size(); ++i)
        {
            BatchHeaderJob job;
            job.input = CanonicalSourcePathKey(discoveredHeaders[i]);
            job.includePath = NormalizePathSlashes(CollapseDotDotPath(discoveredHeaders[i]));
            std::map<std::string, std::string>::const_iterator extraRootIt = extraRootByHeaderKey.find(job.input);
            if (extraRootIt != extraRootByHeaderKey.end())
            {
                job.root = extraRootIt->second;
            }
            else
            {
                job.root = discoveredHeaderRoot.empty() ? DirectoryName(discoveredHeaders[i]) : discoveredHeaderRoot;
            }
            job.rootIndex = discoveredRootIndex;
            headers.push_back(job);
        }

        std::sort(headers.begin(), headers.end(), [](const BatchHeaderJob& a, const BatchHeaderJob& b) {
            if (a.input != b.input) return a.input < b.input;
            if (a.rootIndex != b.rootIndex) return a.rootIndex < b.rootIndex;
            return a.root < b.root;
            });
        headers.erase(std::unique(headers.begin(), headers.end(), [](const BatchHeaderJob& a, const BatchHeaderJob& b) {
            return a.input == b.input;
            }), headers.end());

        if (headers.empty())
        {
            const std::vector<std::string> requestedRootsForEmptyCheck = GetRequestedRootClassNames(opt);
            const bool canParseRootOnly = !requestedRootsForEmptyCheck.empty() && !parseEntryHeader.empty();
            if (!canParseRootOnly)
            {
                if (!opt.includeWhitelistHeader.empty() || !opt.extraTargetListFile.empty())
                {
                    std::cerr << "Batch mode found no reflection targets from whitelist/header target list.\n";
                    if (!opt.includeWhitelistHeader.empty())
                        std::cerr << "  whitelist header: " << opt.includeWhitelistHeader << "\n";
                    if (!opt.extraTargetListFile.empty())
                        std::cerr << "  extra target list: " << opt.extraTargetListFile << "\n";
                }
                else
                {
                    std::cerr << (opt.batchFromDirListFile ? "Batch dir-list mode found no .h/.hpp/.hh/.hxx files.\n" : "Batch mode found no .h/.hpp/.hh/.hxx files.\n");
                }
                return false;
            }
            std::cout << "[root closure] no file emit whitelist targets; parsing umbrella header only for root class closure.\n";
        }

        const std::string outputDir = NormalizePathSlashes(ExpandEnvironmentVariablesInPath(opt.outputHeader));
        if (!ClearBatchOutputDirectoryBeforeEmit(outputDir, clearSafetyRoots, opt))
        {
            return false;
        }
        if (!MakeDirectoryRecursive(outputDir))
        {
            PrintDirectoryCreateFailure("[batch output directory]", outputDir);
            return false;
        }

        std::vector<std::string> aggregateInputs;
        aggregateInputs.reserve(headers.size());
        std::vector<std::string> aggregateIncludePaths;
        aggregateIncludePaths.reserve(headers.size());
        std::set<std::string> whitelist;
        std::map<std::string, BatchHeaderJob> jobBySource;
        std::map<std::string, std::string> outHeaderBySource;
        std::map<std::string, std::string> includeTextBySource;
        std::vector<std::string> generated;
        generated.reserve(headers.size());
        std::set<std::string> usedOutputHeaders;

        for (std::size_t i = 0; i < headers.size(); ++i)
        {
            const std::string input = CanonicalSourcePathKey(headers[i].input);
            const std::string includePath = headers[i].includePath.empty() ? headers[i].input : headers[i].includePath;
            aggregateInputs.push_back(input);
            aggregateIncludePaths.push_back(includePath);
            whitelist.insert(input);
            jobBySource[input] = headers[i];

            const std::string outRel = MakeFlatReflectAutoFileName(includePath, headers[i].root);
            std::string outHeader = JoinPath(outputDir, outRel);
            while (usedOutputHeaders.find(NormalizePathSlashes(outHeader)) != usedOutputHeaders.end())
            {
                const std::string dir = DirectoryName(outHeader);
                const std::string name = FileNameOnly(outHeader);
                outHeader = JoinPath(dir, "dup_" + SanitizeIdentifier(input) + "_" + name);
            }
            usedOutputHeaders.insert(NormalizePathSlashes(outHeader));

            outHeaderBySource[input] = outHeader;
            includeTextBySource[input] = MakeGeneratedHeaderIncludeText(outHeader, includePath);
            generated.push_back(outHeader);
        }

        std::vector<std::string> parseInputs;
        if (!parseEntryHeader.empty())
        {
            // Parse the existing umbrella header itself first.  Extra targets from
            // a txt file that are not already covered by the umbrella header are
            // appended after it so they are visible to libclang without disturbing
            // the umbrella header's original include order.
            parseInputs.push_back(parseEntryHeader);
            std::set<std::string> parseSeen;
            parseSeen.insert(CanonicalSourcePathKey(parseEntryHeader));
            for (std::size_t i = 0; i < extraParseHeaders.size(); ++i)
            {
                const std::string key = CanonicalSourcePathKey(extraParseHeaders[i]);
                if (parseSeen.insert(key).second) parseInputs.push_back(extraParseHeaders[i]);
            }
        }
        else
        {
            parseInputs = aggregateIncludePaths;
        }

        const std::string tempAggregateInput = JoinPath(outputDir, "__reflectgen_batch_all_headers__.h");
        if (!WriteBatchAggregateInputHeader(tempAggregateInput, parseInputs))
        {
            return false;
        }
        std::cout << "[Batch aggregate input] " << tempAggregateInput << "\n";
        std::cout << "[Batch aggregate input] included parse headers: " << parseInputs.size() << "\n";
        std::cout << "[Batch aggregate input] whitelisted target headers: " << aggregateInputs.size() << "\n";

        Options parseOpt = opt;
        parseOpt.batchMode = false;
        parseOpt.inputHeader = tempAggregateInput;
        parseOpt.headerInclude = FileNameOnly(tempAggregateInput);
        // Recursive traversal is mandatory in aggregate mode.  The whitelist is the
        // safety boundary that decides which recursively included files are emitted.
        parseOpt.mainFileOnly = false;

        const std::vector<std::string> requestedRoots = GetRequestedRootClassNames(opt);
        const bool useRootClassClosure = !requestedRoots.empty();

        std::vector<RecordInfo> allRecords;
        if (!ParseAndCollectRecords(parseOpt, false, !useRootClassClosure, whitelist, allRecords))
        {
            return false;
        }

        if (useRootClassClosure)
        {
            const std::vector<RecordInfo> closureRecords = ComputeRootClassClosureRecords(allRecords, requestedRoots);
            const std::map<std::string, const RecordInfo*> globalByName = BuildRecordLookupMap(allRecords);

            std::string closureHeader = JoinPath(outputDir, "root_class_closure_reflect_auto.h");
            Options fileOpt = opt;
            fileOpt.batchMode = false;
            fileOpt.inputHeader = tempAggregateInput;
            fileOpt.outputHeader = closureHeader;
            fileOpt.headerInclude = MakeGeneratedHeaderIncludeText(closureHeader, tempAggregateInput);

            const std::string outDir = DirectoryName(fileOpt.outputHeader);
            if (!outDir.empty() && !MakeDirectoryRecursive(outDir))
            {
                PrintDirectoryCreateFailure("[generated root-closure header output]", outDir);
                return false;
            }

            if (!EmitGeneratedFile(closureRecords, fileOpt, &globalByName))
            {
                return false;
            }

            std::vector<std::string> generatedClosure;
            generatedClosure.push_back(closureHeader);
            std::string aggregate = ExpandEnvironmentVariablesInPath(opt.aggregateHeader);
            if (!IsAbsolutePath(aggregate)) aggregate = JoinPath(outputDir, aggregate);
            if (!EmitAggregateHeader(aggregate, generatedClosure))
            {
                std::cerr << "Failed to generate aggregate header: " << aggregate << "\n";
                return false;
            }
            std::cout << "Generated aggregate: " << aggregate << "\n";
            std::cout << "Generated reflect headers: " << generatedClosure.size() << "\n";
            std::cout << "Total reflected record count: " << closureRecords.size() << "\n";
            return true;
        }

        std::map<std::string, std::vector<RecordInfo> > recordsBySource;
        std::size_t skippedNoSource = 0;
        std::size_t skippedNotWhitelisted = 0;
        for (std::size_t i = 0; i < allRecords.size(); ++i)
        {
            std::string source = CanonicalSourcePathKey(allRecords[i].sourcePath);
            if (source.empty())
            {
                ++skippedNoSource;
                continue;
            }
            if (whitelist.find(source) == whitelist.end())
            {
                ++skippedNotWhitelisted;
                continue;
            }
            recordsBySource[source].push_back(allRecords[i]);
        }

        if (skippedNoSource || skippedNotWhitelisted)
        {
            std::cerr << "[Batch warning] skipped records after grouping: noSource=" << skippedNoSource
                << " notWhitelisted=" << skippedNotWhitelisted << "\n";
        }

        const std::map<std::string, const RecordInfo*> globalByName = BuildRecordLookupMap(allRecords);
        std::size_t totalRecords = 0;
        for (std::size_t i = 0; i < headers.size(); ++i)
        {
            const std::string source = CanonicalSourcePathKey(headers[i].input);
            const std::string outHeader = outHeaderBySource[source];
            const std::string includeText = includeTextBySource[source];
            const std::vector<RecordInfo>& sourceRecords = recordsBySource[source];

            Options fileOpt = opt;
            fileOpt.batchMode = false;
            fileOpt.inputHeader = source;
            fileOpt.outputHeader = outHeader;
            fileOpt.headerInclude = includeText;

            const std::string outDir = DirectoryName(fileOpt.outputHeader);
            if (!outDir.empty() && !MakeDirectoryRecursive(outDir))
            {
                PrintDirectoryCreateFailure("[generated header output]", outDir);
                return false;
            }

            if (gDebugAst)
            {
                std::cout << "[Batch emit] " << source << " -> " << outHeader
                    << " records=" << sourceRecords.size() << "\n";
            }
            if (!EmitGeneratedFile(sourceRecords, fileOpt, &globalByName))
            {
                return false;
            }
            totalRecords += sourceRecords.size();
        }

        std::string aggregate = ExpandEnvironmentVariablesInPath(opt.aggregateHeader);
        if (!IsAbsolutePath(aggregate)) aggregate = JoinPath(outputDir, aggregate);
        if (!EmitAggregateHeader(aggregate, generated))
        {
            std::cerr << "Failed to generate aggregate header: " << aggregate << "\n";
            return false;
        }

        std::cout << "Generated aggregate: " << aggregate << "\n";
        std::cout << "Generated reflect headers: " << generated.size() << "\n";
        std::cout << "Total reflected record count: " << totalRecords << "\n";
        return true;
    }

    class TeeStreamBuf : public std::streambuf
    {
    public:
        TeeStreamBuf(std::streambuf* primary, std::streambuf* secondary)
            : primary_(primary), secondary_(secondary) {}

    protected:
        virtual int overflow(int ch) override
        {
            if (ch == traits_type::eof()) return traits_type::not_eof(ch);
            const char c = static_cast<char>(ch);
            bool ok = true;
            if (primary_ && primary_->sputc(c) == traits_type::eof()) ok = false;
            if (secondary_ && secondary_->sputc(c) == traits_type::eof()) ok = false;
            return ok ? ch : traits_type::eof();
        }

        virtual std::streamsize xsputn(const char* s, std::streamsize n) override
        {
            std::streamsize written = n;
            if (primary_) written = (std::min)(written, primary_->sputn(s, n));
            if (secondary_) written = (std::min)(written, secondary_->sputn(s, n));
            return written;
        }

        virtual int sync() override
        {
            int r1 = primary_ ? primary_->pubsync() : 0;
            int r2 = secondary_ ? secondary_->pubsync() : 0;
            return (r1 == 0 && r2 == 0) ? 0 : -1;
        }

    private:
        std::streambuf* primary_;
        std::streambuf* secondary_;
    };

    std::string FindEarlyOptionValue(int argc, char** argv, const std::string& optionName)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i] ? std::string(argv[i]) : std::string();
            if (a == "--") break;
            if (a == optionName && i + 1 < argc)
            {
                return argv[i + 1] ? std::string(argv[i + 1]) : std::string();
            }
        }
        return std::string();
    }

    bool ApplyHardcodedProjectDefaults(Options& opt)
    {
        if (!opt.batchMode) return true;

        std::string projectPath = NormalizePathSlashes(ExpandEnvironmentVariablesInPath(StripOuterQuotes(opt.vcxprojPath)));
        std::string projectDir;
        if (!projectPath.empty())
        {
            projectDir = DirectoryName(MakeAbsoluteLexicalPath(projectPath));
        }

        if (opt.includeWhitelistHeader.empty() && !projectDir.empty())
        {
            const std::string aqcm = NormalizePathSlashes(JoinPath(projectDir, "inc/aqcm.hpp"));
            if (IsExistingRegularFile(aqcm))
            {
                opt.includeWhitelistHeader = aqcm;
                opt.autoIncludeWhitelistHeader = true;
            }
        }

        if (opt.extraTargetListFile.empty() && !projectDir.empty())
        {
            const std::string targetList = NormalizePathSlashes(JoinPath(projectDir, "reflect_targets.txt"));
            if (IsExistingRegularFile(targetList))
            {
                opt.extraTargetListFile = targetList;
                opt.autoExtraTargetListFile = true;
            }
        }

        if (opt.outputHeader.empty() && !projectDir.empty())
        {
            opt.outputHeader = NormalizePathSlashes(JoinPath(projectDir, "generated_reflect"));
            opt.autoOutputHeader = true;
        }

        if (opt.aggregateHeader.empty())
        {
            opt.aggregateHeader = "project_reflect_auto.h";
        }

        if (opt.logFile.empty() && !opt.outputHeader.empty())
        {
            opt.logFile = NormalizePathSlashes(JoinPath(opt.outputHeader, "reflectgen.log"));
            opt.autoLogFile = true;
        }

        if (opt.batchDirs.empty() && opt.batchDirListFile.empty() && opt.includeWhitelistHeader.empty() && opt.extraTargetListFile.empty())
        {
            std::cerr << "[hardcoded defaults failed] no batch roots, no whitelist header, and no extra target list.\n";
            if (!projectDir.empty())
            {
                std::cerr << "[hardcoded defaults failed] expected whitelist header: "
                    << NormalizePathSlashes(JoinPath(projectDir, "inc/aqcm.hpp")) << "\n";
                std::cerr << "[hardcoded defaults failed] expected extra target list: "
                    << NormalizePathSlashes(JoinPath(projectDir, "reflect_targets.txt")) << "\n";
            }
            std::cerr << "[hardcoded defaults hint] pass --whitelist-from-header <aqcm.hpp>, --extra-target-list <targets.txt>, or --batch-dir/--batch-dir-list explicitly.\n";
            return false;
        }

        if (opt.outputHeader.empty())
        {
            std::cerr << "[hardcoded defaults failed] no output directory. Pass -o <generated_reflect_dir>.\n";
            return false;
        }

        return true;
    }

    struct ScopedReflectGenLog
    {
        std::ofstream file;
        std::unique_ptr<TeeStreamBuf> coutTee;
        std::unique_ptr<TeeStreamBuf> cerrTee;
        std::streambuf* oldCout = NULL;
        std::streambuf* oldCerr = NULL;
        std::string path;

        bool open(const std::string& rawPath)
        {
            path = NormalizePathSlashes(ExpandEnvironmentVariablesInPath(StripOuterQuotes(rawPath)));
            if (path.empty()) return false;

            const std::string dir = DirectoryName(path);
            if (!dir.empty() && !MakeDirectoryRecursive(dir))
            {
                PrintDirectoryCreateFailure("[log]", dir);
                return false;
            }

            errno = 0;
            file.open(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
            if (!file)
            {
                PrintFileOpenFailure("[log]", path);
                return false;
            }

            oldCout = std::cout.rdbuf();
            oldCerr = std::cerr.rdbuf();
            coutTee.reset(new TeeStreamBuf(oldCout, file.rdbuf()));
            cerrTee.reset(new TeeStreamBuf(oldCerr, file.rdbuf()));
            std::cout.rdbuf(coutTee.get());
            std::cerr.rdbuf(cerrTee.get());

            std::cout << "[log] ReflectGen log file: " << path << "\n";
            return true;
        }

        void close()
        {
            std::cout.flush();
            std::cerr.flush();
            if (oldCout) std::cout.rdbuf(oldCout);
            if (oldCerr) std::cerr.rdbuf(oldCerr);
            oldCout = NULL;
            oldCerr = NULL;
            if (file) file.flush();
        }

        ~ScopedReflectGenLog()
        {
            close();
        }
    };

}

int main(int argc, char** argv)
{
    ScopedReflectGenLog log;
    std::string earlyLogFile = FindEarlyOptionValue(argc, argv, "--log-file");
    if (earlyLogFile.empty()) earlyLogFile = FindEarlyOptionValue(argc, argv, "--log");
    if (!earlyLogFile.empty())
    {
        log.open(earlyLogFile);
    }

    Options opt;
    if (!ParseCommandLine(argc, argv, opt)) return 1;
    if (!ApplyHardcodedProjectDefaults(opt)) return 1;

    if (!opt.logFile.empty() && earlyLogFile.empty())
    {
        log.open(opt.logFile);
    }

    if (opt.autoIncludeWhitelistHeader)
    {
        std::cout << "[hardcoded default] whitelist header: " << opt.includeWhitelistHeader << "\n";
    }
    if (opt.autoExtraTargetListFile)
    {
        std::cout << "[hardcoded default] extra target list: " << opt.extraTargetListFile << "\n";
    }
    if (opt.autoOutputHeader)
    {
        std::cout << "[hardcoded default] output directory: " << opt.outputHeader << "\n";
    }
    if (opt.autoLogFile)
    {
        std::cout << "[hardcoded default] log file: " << opt.logFile << "\n";
    }

    if (!ApplyVcxprojSettings(opt)) return 2;

    if (opt.batchMode)
    {
        return RunBatchMode(opt) ? 0 : 3;
    }

    std::size_t recordCount = 0;
    if (!GenerateOneHeader(opt, opt.inputHeader, opt.outputHeader, opt.headerInclude, recordCount))
    {
        return 3;
    }

    return 0;
}
#if defined(REFLECTGEN_RESTORE_MAX_MACRO_)
#pragma pop_macro("max")
#undef REFLECTGEN_RESTORE_MAX_MACRO_
#endif
#if defined(REFLECTGEN_RESTORE_MIN_MACRO_)
#pragma pop_macro("min")
#undef REFLECTGEN_RESTORE_MIN_MACRO_
#endif

