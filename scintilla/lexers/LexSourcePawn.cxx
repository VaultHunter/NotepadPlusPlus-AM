// Scintilla source code edit control
/** @file LexSourcePawn.cxx
 ** Lexer for SourcePawn.
 **/
// Copyright 1998-2005 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

// NPPSTART Joce 10/11/10 Scintilla_precomp_headers
//#include "precompiled_headers.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

#ifdef _MSC_VER
#pragma warning(disable: 4786)
#endif
#ifdef __BORLANDC__
// Borland C++ displays warnings in vector header without this
#pragma option -w-ccc -w-rch
#endif

#include <string>
#include <vector>
#include <map>
#include <algorithm>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

#include "PropSetSimple.h"
#include "WordList.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "StyleContext.h"
#include "CharacterSet.h"
#include "LexerModule.h"
#include "OptionSet.h"
// NPPEND

#ifdef SCI_NAMESPACE
using namespace Scintilla;
#endif

static bool IsSpaceEquiv(int state) 
{
	return (state <= SCE_SOURCEPAWN_COMMENTDOC) ||
		// including SCE_SOURCEPAWN_DEFAULT, SCE_SOURCEPAWN_COMMENT, SCE_SOURCEPAWN_COMMENTLINE
		(state == SCE_SOURCEPAWN_COMMENTLINEDOC) || (state == SCE_SOURCEPAWN_COMMENTDOCKEYWORD) ||
		(state == SCE_SOURCEPAWN_COMMENTDOCKEYWORDERROR);
}

static std::string GetRestOfLine(LexAccessor &styler, int start, bool allowSpace) {
	std::string restOfLine;
	int i =0;
	char ch = styler.SafeGetCharAt(start + i, '\n');
	while ((ch != '\r') && (ch != '\n')) {
		if (allowSpace || (ch != ' '))
			restOfLine += ch;
		i++;
		ch = styler.SafeGetCharAt(start + i, '\n');
	}
	return restOfLine;
}

static bool IsStreamCommentStyle(int style) {
	return style == SCE_SOURCEPAWN_COMMENT ||
		style == SCE_SOURCEPAWN_COMMENTDOC ||
		style == SCE_SOURCEPAWN_COMMENTDOCKEYWORD ||
		style == SCE_SOURCEPAWN_COMMENTDOCKEYWORDERROR;
}

static std::vector<std::string> Tokenize(const std::string &s) {
	// Break into space separated tokens
	std::string word;
	std::vector<std::string> tokens;
	for (const char *cp = s.c_str(); *cp; cp++) {
		if ((*cp == ' ') || (*cp == '\t')) {
			if (!word.empty()) {
				tokens.push_back(word);
				word = "";
			}
		} else {
			word += *cp;
		}
	}
	if (!word.empty()) {
		tokens.push_back(word);
	}
	return tokens;
}

struct PPDefinition {
	int line;
	std::string key;
	std::string value;
	PPDefinition(int line_, const std::string &key_, const std::string &value_) :
		line(line_), key(key_), value(value_) {
	}
};

class LinePPState {
	int state;
	int ifTaken;
	int level;
	bool ValidLevel() const {
		return level >= 0 && level < 32;
	}
	int maskLevel() const {
		return 1 << level;
	}
public:
	LinePPState() : state(0), ifTaken(0), level(-1) {
	}
	bool IsInactive() const {
		return state != 0;
	}
	bool CurrentIfTaken() {
		return (ifTaken & maskLevel()) != 0;
	}
	void StartSection(bool on) {
		level++;
		if (ValidLevel()) {
			if (on) {
				state &= ~maskLevel();
				ifTaken |= maskLevel();
			} else {
				state |= maskLevel();
				ifTaken &= ~maskLevel();
			}
		}
	}
	void EndSection() {
		if (ValidLevel()) {
			state &= ~maskLevel();
			ifTaken &= ~maskLevel();
		}
		level--;
	}
	void InvertCurrentLevel() {
		if (ValidLevel()) {
			state ^= maskLevel();
			ifTaken |= maskLevel();
		}
	}
};

// Hold the preprocessor state for each line seen.
// Currently one entry per line but could become sparse with just one entry per preprocessor line.
class PPStates {
	std::vector<LinePPState> vlls;
public:
	LinePPState ForLine(int line) {
		if ((line > 0) && (vlls.size() > static_cast<size_t>(line))) {
			return vlls[line];
		} else {
			return LinePPState();
		}
	}
	void Add(int line, LinePPState lls) {
		vlls.resize(line+1);
		vlls[line] = lls;
	}
};

// An individual named option for use in an OptionSet

// Options used for LexerSourcePawn
struct OptionsSourcePawn {
	bool stylingWithinPreprocessor;
	bool identifiersAllowDollars;
	bool trackPreprocessor;
	bool updatePreprocessor;
	bool fold;
	bool foldComment;
	bool foldCommentExplicit;
	bool foldPreprocessor;
	bool foldCompact;
	bool foldAtElse;
	OptionsSourcePawn() {
		stylingWithinPreprocessor = false;
		identifiersAllowDollars = true;
		trackPreprocessor = true;
		updatePreprocessor = true;
		fold = false;
		foldComment = false;
		foldCommentExplicit = true;
		foldPreprocessor = false;
		foldCompact = false;
		foldAtElse = false;
	}
};

static const char *const sourcepawnWordLists[] = {
            "Native keywords",
            "Forward keywords",
			"Statements keywords",
			"Constants keywords",
			"user1",
            "Documentation comment keywords",
            "Preprocessor definitions",
            0,
};

struct OptionSetSourcePawn : public OptionSet<OptionsSourcePawn> {
	OptionSetSourcePawn() {
		DefineProperty("styling.within.preprocessor", &OptionsSourcePawn::stylingWithinPreprocessor,
			"For C++ code, determines whether all preprocessor code is styled in the "
			"preprocessor style (0, the default) or only from the initial # to the end "
			"of the command word(1).");

		DefineProperty("lexer.cpp.allow.dollars", &OptionsSourcePawn::identifiersAllowDollars,
			"Set to 0 to disallow the '$' character in identifiers with the cpp lexer.");

		DefineProperty("lexer.cpp.track.preprocessor", &OptionsSourcePawn::trackPreprocessor,
			"Set to 1 to interpret #if/#else/#endif to grey out code that is not active.");

		DefineProperty("lexer.cpp.update.preprocessor", &OptionsSourcePawn::updatePreprocessor,
			"Set to 1 to update preprocessor definitions when #define found.");

		DefineProperty("fold", &OptionsSourcePawn::fold);

		DefineProperty("fold.comment", &OptionsSourcePawn::foldComment,
			"This option enables folding multi-line comments and explicit fold points when using the C++ lexer. "
			"Explicit fold points allows adding extra folding by placing a //{ comment at the start and a //} "
			"at the end of a section that should fold.");

		DefineProperty("fold.cpp.comment.explicit", &OptionsSourcePawn::foldCommentExplicit,
			"Set this property to 0 to disable folding explicit fold points when fold.comment=1.");

		DefineProperty("fold.preprocessor", &OptionsSourcePawn::foldPreprocessor,
			"This option enables folding preprocessor directives when using the C++ lexer. "
			"Includes C#'s explicit #region and #endregion folding directives.");

		DefineProperty("fold.compact", &OptionsSourcePawn::foldCompact);

		DefineProperty("fold.at.else", &OptionsSourcePawn::foldAtElse,
			"This option enables C++ folding on a \"} else {\" line of an if statement.");

		DefineWordListSets(sourcepawnWordLists);
	}
};

class LexerSourcePawn : public ILexer {
	bool caseSensitive;
	CharacterSet setWord;
	CharacterSet setNegationOp;
	CharacterSet setArithmethicOp;
	CharacterSet setRelOp;
	CharacterSet setLogicalOp;
	PPStates vlls;
	std::vector<PPDefinition> ppDefineHistory;
	PropSetSimple props;
	WordList keywordsNative;
	WordList keywordsForward;
	WordList keywordsStatement;
	WordList keywordsConstant;
	WordList keywords;
	WordList keywords2;
	WordList ppDefinitions;
	std::map<std::string, std::string> preprocessorDefinitionsStart;
	OptionsSourcePawn options;
	OptionSetSourcePawn osSourcePawn;
public:
	LexerSourcePawn(bool caseSensitive_) :
		caseSensitive(caseSensitive_),
		setWord(CharacterSet::setAlphaNum, "._", 0x80, true),
		setNegationOp(CharacterSet::setNone, "!"),
		setArithmethicOp(CharacterSet::setNone, "+-/*%"),
		setRelOp(CharacterSet::setNone, "=!<>"),
		setLogicalOp(CharacterSet::setNone, "|&") {
	}
	~LexerSourcePawn() {
	}
	void SCI_METHOD Release() {
		delete this;
	}
	int SCI_METHOD Version() const {
		return lvOriginal;
	}
	const char * SCI_METHOD PropertyNames() {
		return osSourcePawn.PropertyNames();
	}
	int SCI_METHOD PropertyType(const char *name) {
		return osSourcePawn.PropertyType(name);
	}
	const char * SCI_METHOD DescribeProperty(const char *name) {
		return osSourcePawn.DescribeProperty(name);
	}
	int SCI_METHOD PropertySet(const char *key, const char *val);
	const char * SCI_METHOD DescribeWordListSets() {
		return osSourcePawn.DescribeWordListSets();
	}
	int SCI_METHOD WordListSet(int n, const char *wl);
	void SCI_METHOD Lex(unsigned int startPos, int length, int initStyle, IDocument *pAccess);
	void SCI_METHOD Fold(unsigned int startPos, int length, int initStyle, IDocument *pAccess);

	void * SCI_METHOD PrivateCall(int, void *) {
		return 0;
	}

	static ILexer *LexerFactorySourcePawn() {
		return new LexerSourcePawn(true);
	}

	void EvaluateTokens(std::vector<std::string> &tokens);
	bool EvaluateExpression(const std::string &expr, const std::map<std::string, std::string> &preprocessorDefinitions);
};

int SCI_METHOD LexerSourcePawn::PropertySet(const char *key, const char *val) {
	if (osSourcePawn.PropertySet(&options, key, val)) {
		return 0;
	}
	return -1;
}

int SCI_METHOD LexerSourcePawn::WordListSet(int n, const char *wl) {
	WordList *wordListN = 0;
	switch (n) {
	case 0:
		wordListN = &keywordsNative;
		break;
	case 1:
		wordListN = &keywordsForward;
		break;
	case 2:
		wordListN = &keywordsStatement;
		break;
	case 3:
		wordListN = &keywordsConstant;
		break;
	case 4:
		wordListN = &keywords;
		break;
	case 5:
		wordListN = &keywords2;
		break;
	case 6:
		wordListN = &ppDefinitions;
		break;
	}
	int firstModification = -1;
	if (wordListN) {
		WordList wlNew;
		wlNew.Set(wl);
		if (*wordListN != wlNew) {
			wordListN->Set(wl);
			firstModification = 0;
			if (n == 6) {
				// Rebuild preprocessorDefinitions
				preprocessorDefinitionsStart.clear();
				for (int nDefinition = 0; nDefinition < ppDefinitions.len; nDefinition++) {
					char *cpDefinition = ppDefinitions.words[nDefinition];
					char *cpEquals = strchr(cpDefinition, '=');
					if (cpEquals) {
						std::string name(cpDefinition, cpEquals - cpDefinition);
						std::string val(cpEquals+1);
						preprocessorDefinitionsStart[name] = val;
					} else {
						std::string name(cpDefinition);
						std::string val("1");
						preprocessorDefinitionsStart[name] = val;
					}
				}
			}
		}
	}
	return firstModification;
}

// Functor used to truncate history
struct After {
	int line;
	After(int line_) : line(line_) {}
	bool operator() (PPDefinition &p) const {
		return p.line > line;
	}
};

void SCI_METHOD LexerSourcePawn::Lex(unsigned int startPos, int length, int initStyle, IDocument *pAccess) {
	LexAccessor styler(pAccess);

	CharacterSet setOKBeforeRE(CharacterSet::setNone, "([{=,:;!%^&*|?~+-");
	CharacterSet setCouldBePostOp(CharacterSet::setNone, "+-");

	CharacterSet setDoxygen(CharacterSet::setAlpha, "$@\\&<>#{}[]");

	CharacterSet setWordStart(CharacterSet::setAlpha, "_", 0x80, true);
	CharacterSet setWord(CharacterSet::setAlphaNum, "._", 0x80, true);

	if (options.identifiersAllowDollars) {
		setWordStart.Add('$');
		setWord.Add('$');
	}

	int chPrevNonWhite = ' ';
	int visibleChars = 0;
	int styleBeforeDCKeyword = SCE_SOURCEPAWN_DEFAULT;
	bool continuationLine = false;
	bool isIncludePreprocessor = false;

	int lineCurrent = styler.GetLine(startPos);
	if (initStyle == SCE_SOURCEPAWN_PREPROCESSOR) {
		// Set continuationLine if last character of previous line is '\'
		if (lineCurrent > 0) {
			int chBack = styler.SafeGetCharAt(startPos-1, 0);
			int chBack2 = styler.SafeGetCharAt(startPos-2, 0);
			int lineEndChar = '!';
			if (chBack2 == '\r' && chBack == '\n') {
				lineEndChar = styler.SafeGetCharAt(startPos-3, 0);
			} else if (chBack == '\n' || chBack == '\r') {
				lineEndChar = chBack2;
			}
			continuationLine = lineEndChar == '\\';
		}
	}

	// look back to set chPrevNonWhite properly for better regex colouring
	if (startPos > 0) {
		int back = startPos;
		while (--back && IsSpaceEquiv(styler.StyleAt(back)))
			;
		if (styler.StyleAt(back) == SCE_SOURCEPAWN_OPERATOR) {
			chPrevNonWhite = styler.SafeGetCharAt(back);
		}
	}

	StyleContext sc(startPos, length, initStyle, styler, 0x7f);
	LinePPState preproc = vlls.ForLine(lineCurrent);

	bool definitionsChanged = false;

	// Truncate ppDefineHistory before current line

	if (!options.updatePreprocessor)
		ppDefineHistory.clear();

	std::vector<PPDefinition>::iterator itInvalid = std::find_if(ppDefineHistory.begin(), ppDefineHistory.end(), After(lineCurrent-1));
	if (itInvalid != ppDefineHistory.end()) {
		ppDefineHistory.erase(itInvalid, ppDefineHistory.end());
		definitionsChanged = true;
	}

	std::map<std::string, std::string> preprocessorDefinitions = preprocessorDefinitionsStart;
	for (std::vector<PPDefinition>::iterator itDef = ppDefineHistory.begin(); itDef != ppDefineHistory.end(); ++itDef) {
		preprocessorDefinitions[itDef->key] = itDef->value;
	}

	const int maskActivity = 0x3F;

	int activitySet = preproc.IsInactive() ? 0x40 : 0;

	for (; sc.More(); sc.Forward()) {

		if (sc.atLineStart) {
			if (sc.state == SCE_SOURCEPAWN_STRING) {
				// Prevent SCE_SOURCEPAWN_STRINGEOL from leaking back to previous line which
				// ends with a line continuation by locking in the state upto this position.
				sc.SetState(SCE_SOURCEPAWN_STRING);
			}
			// Reset states to begining of colourise so no surprises
			// if different sets of lines lexed.
			visibleChars = 0;
			isIncludePreprocessor = false;
			if (preproc.IsInactive()) {
				activitySet = 0x40;
				sc.SetState(sc.state | activitySet);
			}
			if (activitySet) {
				if (sc.ch == '#') {
					if (sc.Match("#else") || sc.Match("#end") || sc.Match("#if")) {
						//activitySet = 0;
					}
				}
			}
		}

		if (sc.atLineEnd) {
			lineCurrent++;
			vlls.Add(lineCurrent, preproc);
		}

		// Handle line continuation generically.
		if (sc.ch == '\\') {
			if (sc.chNext == '\n' || sc.chNext == '\r') {
				sc.Forward();
				if (sc.ch == '\r' && sc.chNext == '\n') {
					sc.Forward();
				}
				continuationLine = true;
				continue;
			}
		}

		const bool atLineEndBeforeSwitch = sc.atLineEnd;

		// Determine if the current state should terminate.
		switch (sc.state & maskActivity) {
			case SCE_SOURCEPAWN_OPERATOR:
				sc.SetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
				break;
			case SCE_SOURCEPAWN_NUMBER:
				// We accept almost anything because of hex. and number suffixes
				if (!setWord.Contains(sc.ch)) {
					sc.SetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
				}
				break;
			case SCE_SOURCEPAWN_IDENTIFIER:
				if (!setWord.Contains(sc.ch) || (sc.ch == '.')) {
					char s[1000];
					if (caseSensitive) {
						sc.GetCurrent(s, sizeof(s));
					} else {
						sc.GetCurrentLowered(s, sizeof(s));
					}
					if (keywordsNative.InList(s)) {
						sc.ChangeState(SCE_SOURCEPAWN_NATIVE|activitySet);
					} else if (keywordsForward.InList(s)) {
						sc.ChangeState(SCE_SOURCEPAWN_FORWARD|activitySet);
					} else if (keywordsStatement.InList(s)) {
						sc.ChangeState(SCE_SOURCEPAWN_STATEMENT|activitySet);
					} else if (keywordsConstant.InList(s)) {
						sc.ChangeState(SCE_SOURCEPAWN_CONSTANT|activitySet);
					} else if (keywords.InList(s)) {
						sc.ChangeState(SCE_SOURCEPAWN_WORD|activitySet);
					} else if (keywords2.InList(s)) {
						sc.ChangeState(SCE_SOURCEPAWN_WORD2|activitySet);
					}

					sc.SetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
				}
				break;
			case SCE_SOURCEPAWN_PREPROCESSOR:
				if (sc.atLineStart && !continuationLine) {
					sc.SetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
				} else if (options.stylingWithinPreprocessor) {
					if (IsASpace(sc.ch)) {
						sc.SetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
					}
				} else {
					if (sc.Match('/', '*') || sc.Match('/', '/')) {
						sc.SetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
					}
				}
				break;
			case SCE_SOURCEPAWN_COMMENT:
				if (sc.Match('*', '/')) {
					sc.Forward();
					sc.ForwardSetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
				}
				break;
			case SCE_SOURCEPAWN_COMMENTDOC:
				if (sc.Match('*', '/')) {
					sc.Forward();
					sc.ForwardSetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
				} else if (sc.ch == '@' || sc.ch == '\\') { // JavaDoc and Doxygen support
					// Verify that we have the conditions to mark a comment-doc-keyword
					if ((IsASpace(sc.chPrev) || sc.chPrev == '*') && (!IsASpace(sc.chNext))) {
						styleBeforeDCKeyword = SCE_SOURCEPAWN_COMMENTDOC;
						sc.SetState(SCE_SOURCEPAWN_COMMENTDOCKEYWORD|activitySet);
					}
				}
				break;
			case SCE_SOURCEPAWN_COMMENTLINE:
				if (sc.atLineStart) {
					sc.SetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
				}
				break;
			case SCE_SOURCEPAWN_COMMENTLINEDOC:
				if (sc.atLineStart) {
					sc.SetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
				} else if (sc.ch == '@' || sc.ch == '\\') { // JavaDoc and Doxygen support
					// Verify that we have the conditions to mark a comment-doc-keyword
					if ((IsASpace(sc.chPrev) || sc.chPrev == '/' || sc.chPrev == '!') && (!IsASpace(sc.chNext))) {
						styleBeforeDCKeyword = SCE_SOURCEPAWN_COMMENTLINEDOC;
						sc.SetState(SCE_SOURCEPAWN_COMMENTDOCKEYWORD|activitySet);
					}
				}
				break;
			case SCE_SOURCEPAWN_COMMENTDOCKEYWORD:
				if ((styleBeforeDCKeyword == SCE_SOURCEPAWN_COMMENTDOC) && sc.Match('*', '/')) {
					sc.ChangeState(SCE_SOURCEPAWN_COMMENTDOCKEYWORDERROR);
					sc.Forward();
					sc.ForwardSetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
				} else if (!setDoxygen.Contains(sc.ch)) {
					char s[100];
					if (caseSensitive) {
						sc.GetCurrent(s, sizeof(s));
					} else {
						sc.GetCurrentLowered(s, sizeof(s));
					}
					if (!IsASpace(sc.ch) || !keywords2.InList(s + 1)) {
						sc.ChangeState(SCE_SOURCEPAWN_COMMENTDOCKEYWORDERROR|activitySet);
					}
					sc.SetState(styleBeforeDCKeyword);
				}
				break;
			case SCE_SOURCEPAWN_STRING:
				if (sc.atLineEnd) {
					sc.ChangeState(SCE_SOURCEPAWN_STRINGEOL|activitySet);
				} else if (isIncludePreprocessor) {
					if (sc.ch == '>') {
						sc.ForwardSetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
						isIncludePreprocessor = false;
					}
				} else if (sc.ch == '\\' || sc.ch == '^') {
					if (sc.chNext == '\"' || sc.chNext == '\'' || sc.chNext == '\\') {
						sc.Forward();
					}
				} else if (sc.ch == '\"') {
					sc.ForwardSetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
				}
				break;
			case SCE_SOURCEPAWN_CHARACTER:
				if (sc.atLineEnd) {
					sc.ChangeState(SCE_SOURCEPAWN_STRINGEOL|activitySet);
				} else if (sc.ch == '\\') {
					if (sc.chNext == '\"' || sc.chNext == '\'' || sc.chNext == '\\') {
						sc.Forward();
					}
				} else if (sc.ch == '\'') {
					sc.ForwardSetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
				}
				break;
			case SCE_SOURCEPAWN_STRINGEOL:
				if (sc.atLineStart) {
					sc.SetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
				}
				break;
			case SCE_SOURCEPAWN_VERBATIM:
				if (sc.ch == '\"') {
					if (sc.chNext == '\"') {
						sc.Forward();
					} else {
						sc.ForwardSetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
					}
				}
				break;
		}

		if (sc.atLineEnd && !atLineEndBeforeSwitch) {
			// State exit processing consumed characters up to end of line.
			lineCurrent++;
			vlls.Add(lineCurrent, preproc);
		}

		// Determine if a new state should be entered.
		if ((sc.state & maskActivity) == SCE_SOURCEPAWN_DEFAULT) {
			if (sc.Match('@', '\"')) {
				sc.SetState(SCE_SOURCEPAWN_VERBATIM|activitySet);
				sc.Forward();
			} else if (IsADigit(sc.ch) || (sc.ch == '.' && IsADigit(sc.chNext))) {
				sc.SetState(SCE_SOURCEPAWN_NUMBER|activitySet);
			} else if (setWordStart.Contains(sc.ch) || (sc.ch == '@')) {
				sc.SetState(SCE_SOURCEPAWN_IDENTIFIER|activitySet);
			} else if (sc.Match('/', '*')) {
				if (sc.Match("/**") || sc.Match("/*!")) {	// Support of Qt/Doxygen doc. style
					sc.SetState(SCE_SOURCEPAWN_COMMENTDOC|activitySet);
				} else {
					sc.SetState(SCE_SOURCEPAWN_COMMENT|activitySet);
				}
				sc.Forward();	// Eat the * so it isn't used for the end of the comment
			} else if (sc.Match('/', '/')) {
				if ((sc.Match("///") && !sc.Match("////")) || sc.Match("//!"))
					// Support of Qt/Doxygen doc. style
					sc.SetState(SCE_SOURCEPAWN_COMMENTLINEDOC|activitySet);
				else
					sc.SetState(SCE_SOURCEPAWN_COMMENTLINE|activitySet);
			} else if (sc.ch == '\"') {
				sc.SetState(SCE_SOURCEPAWN_STRING|activitySet);
				isIncludePreprocessor = false;	// ensure that '>' won't end the string
			} else if (isIncludePreprocessor && sc.ch == '<') {
				sc.SetState(SCE_SOURCEPAWN_STRING|activitySet);
			} else if (sc.ch == '\'') {
				sc.SetState(SCE_SOURCEPAWN_CHARACTER|activitySet);
			} else if (sc.ch == '#' && visibleChars == 0) {
				// Preprocessor commands are alone on their line
				sc.SetState(SCE_SOURCEPAWN_PREPROCESSOR|activitySet);
				// Skip whitespace between # and preprocessor word
				do {
					sc.Forward();
				} while ((sc.ch == ' ' || sc.ch == '\t') && sc.More());
				if (sc.atLineEnd) {
					sc.SetState(SCE_SOURCEPAWN_DEFAULT|activitySet);
				} else if (sc.Match("include")) {
					isIncludePreprocessor = true;
				} else {
					if (options.trackPreprocessor) {
						if (sc.Match("ifdef") || sc.Match("ifndef")) {
							bool isIfDef = sc.Match("ifdef");
							int i = isIfDef ? 5 : 6;
							std::string restOfLine = GetRestOfLine(styler, sc.currentPos + i + 1, false);
							bool foundDef = preprocessorDefinitions.find(restOfLine) != preprocessorDefinitions.end();
							preproc.StartSection(isIfDef == foundDef);
						} else if (sc.Match("if")) {
							std::string restOfLine = GetRestOfLine(styler, sc.currentPos + 2, true);
							bool ifGood = EvaluateExpression(restOfLine, preprocessorDefinitions);
							preproc.StartSection(ifGood);
						} else if (sc.Match("else")) {
							if (!preproc.CurrentIfTaken()) {
								preproc.InvertCurrentLevel();
								activitySet = preproc.IsInactive() ? 0x40 : 0;
								if (!activitySet)
									sc.ChangeState(SCE_SOURCEPAWN_PREPROCESSOR|activitySet);
							} else if (!preproc.IsInactive()) {
								preproc.InvertCurrentLevel();
								activitySet = preproc.IsInactive() ? 0x40 : 0;
								if (!activitySet)
									sc.ChangeState(SCE_SOURCEPAWN_PREPROCESSOR|activitySet);
							}
						} else if (sc.Match("elif")) {
							// Ensure only one chosen out of #if .. #elif .. #elif .. #else .. #endif
							if (!preproc.CurrentIfTaken()) {
								// Similar to #if
								std::string restOfLine = GetRestOfLine(styler, sc.currentPos + 2, true);
								bool ifGood = EvaluateExpression(restOfLine, preprocessorDefinitions);
								if (ifGood) {
									preproc.InvertCurrentLevel();
									activitySet = preproc.IsInactive() ? 0x40 : 0;
									if (!activitySet)
										sc.ChangeState(SCE_SOURCEPAWN_PREPROCESSOR|activitySet);
								}
							} else if (!preproc.IsInactive()) {
								preproc.InvertCurrentLevel();
								activitySet = preproc.IsInactive() ? 0x40 : 0;
								if (!activitySet)
									sc.ChangeState(SCE_SOURCEPAWN_PREPROCESSOR|activitySet);
							}
						} else if (sc.Match("endif")) {
							preproc.EndSection();
							activitySet = preproc.IsInactive() ? 0x40 : 0;
							sc.ChangeState(SCE_SOURCEPAWN_PREPROCESSOR|activitySet);
						} else if (sc.Match("define")) {
							if (options.updatePreprocessor && !preproc.IsInactive()) {
								std::string restOfLine = GetRestOfLine(styler, sc.currentPos + 6, true);
								if (restOfLine.find(")") == std::string::npos) {	// Don't handle macros with arguments
									std::vector<std::string> tokens = Tokenize(restOfLine);
									std::string key;
									std::string value("1");
									if (tokens.size() >= 1) {
										key = tokens[0];
										if (tokens.size() >= 2) {
											value = tokens[1];
										}
										preprocessorDefinitions[key] = value;
										ppDefineHistory.push_back(PPDefinition(lineCurrent, key, value));
										definitionsChanged = true;
									}
								}
							}
						}
					}
				}
			} else if (isoperator(static_cast<char>(sc.ch))) {
				sc.SetState(SCE_SOURCEPAWN_OPERATOR|activitySet);
			}
		}

		if (!IsASpace(sc.ch) && !IsSpaceEquiv(sc.state)) {
			chPrevNonWhite = sc.ch;
			visibleChars++;
		}
		continuationLine = false;
	}
	if (definitionsChanged)
		styler.ChangeLexerState(startPos, startPos + length);
	sc.Complete();
	styler.Flush();
}

// Store both the current line's fold level and the next lines in the
// level store to make it easy to pick up with each increment
// and to make it possible to fiddle the current level for "} else {".

void SCI_METHOD LexerSourcePawn::Fold(unsigned int startPos, int length, int initStyle, IDocument *pAccess) {

	if (!options.fold)
		return;

	LexAccessor styler(pAccess);

	unsigned int endPos = startPos + length;
	int visibleChars = 0;
	int lineCurrent = styler.GetLine(startPos);
	int levelCurrent = SC_FOLDLEVELBASE;
	if (lineCurrent > 0)
		levelCurrent = styler.LevelAt(lineCurrent-1) >> 16;
	int levelMinCurrent = levelCurrent;
	int levelNext = levelCurrent;
	char chNext = styler[startPos];
	int styleNext = styler.StyleAt(startPos);
	int style = initStyle;
	for (unsigned int i = startPos; i < endPos; i++) {
		char ch = chNext;
		chNext = styler.SafeGetCharAt(i + 1);
		int stylePrev = style;
		style = styleNext;
		styleNext = styler.StyleAt(i + 1);
		bool atEOL = (ch == '\r' && chNext != '\n') || (ch == '\n');
		if (options.foldComment && IsStreamCommentStyle(style)) {
			if (!IsStreamCommentStyle(stylePrev) && (stylePrev != SCE_SOURCEPAWN_COMMENTLINEDOC)) {
				levelNext++;
			} else if (!IsStreamCommentStyle(styleNext) && (styleNext != SCE_SOURCEPAWN_COMMENTLINEDOC) && !atEOL) {
				// Comments don't end at end of line and the next character may be unstyled.
				levelNext--;
			}
		}
		if (options.foldComment && options.foldCommentExplicit && (style == SCE_SOURCEPAWN_COMMENTLINE)) {
			if ((ch == '/') && (chNext == '/')) {
				char chNext2 = styler.SafeGetCharAt(i + 2);
				if (chNext2 == '{') {
					levelNext++;
				} else if (chNext2 == '}') {
					levelNext--;
				}
			}
		}
		if (options.foldPreprocessor && (style == SCE_SOURCEPAWN_PREPROCESSOR)) {
			if (ch == '#') {
				unsigned int j = i + 1;
				while ((j < endPos) && IsASpaceOrTab(styler.SafeGetCharAt(j))) {
					j++;
				}
				if (styler.Match(j, "region") || styler.Match(j, "if")) {
					levelNext++;
				} else if (styler.Match(j, "end")) {
					levelNext--;
				}
			}
		}
		if (style == SCE_SOURCEPAWN_OPERATOR) {
			if (ch == '{') {
				// Measure the minimum before a '{' to allow
				// folding on "} else {"
				if (levelMinCurrent > levelNext) {
					levelMinCurrent = levelNext;
				}
				levelNext++;
			} else if (ch == '}') {
				levelNext--;
			}
		}
		if (!IsASpace(ch))
			visibleChars++;
		if (atEOL || (i == endPos-1)) {
			int levelUse = levelCurrent;
			if (options.foldAtElse) {
				levelUse = levelMinCurrent;
			}
			int lev = levelUse | levelNext << 16;
			if (visibleChars == 0 && options.foldCompact)
				lev |= SC_FOLDLEVELWHITEFLAG;
			if (levelUse < levelNext)
				lev |= SC_FOLDLEVELHEADERFLAG;
			if (lev != styler.LevelAt(lineCurrent)) {
				styler.SetLevel(lineCurrent, lev);
			}
			lineCurrent++;
			levelCurrent = levelNext;
			levelMinCurrent = levelCurrent;
			if (atEOL && (i == static_cast<unsigned int>(styler.Length()-1))) {
				// There is an empty line at end of file so give it same level and empty
				styler.SetLevel(lineCurrent, (levelCurrent | levelCurrent << 16) | SC_FOLDLEVELWHITEFLAG);
			}
			visibleChars = 0;
		}
	}
}

void LexerSourcePawn::EvaluateTokens(std::vector<std::string> &tokens) {

	// Evaluate defined() statements to either 0 or 1
	for (size_t i=0; (i+2)<tokens.size();) {
		if ((tokens[i] == "defined") && (tokens[i+1] == "(")) {
			const char *val = "0";
			if (tokens[i+2] == ")") {
				// defined()
				tokens.erase(tokens.begin() + i + 1, tokens.begin() + i + 3);
			} else if (((i+2)<tokens.size()) && (tokens[i+3] == ")")) {
				// defined(<int>)
				tokens.erase(tokens.begin() + i + 1, tokens.begin() + i + 4);
				val = "1";
			}
			tokens[i] = val;
		} else {
			i++;
		}
	}

	// Find bracketed subexpressions and recurse on them
	std::vector<std::string>::iterator itBracket = std::find(tokens.begin(), tokens.end(), "(");
	std::vector<std::string>::iterator itEndBracket = std::find(tokens.begin(), tokens.end(), ")");
	while ((itBracket != tokens.end()) && (itEndBracket != tokens.end()) && (itEndBracket > itBracket)) {
		std::vector<std::string> inBracket(itBracket + 1, itEndBracket);
		EvaluateTokens(inBracket);

		// The insertion is done before the removal because there were failures with the opposite approach
		tokens.insert(itBracket, inBracket.begin(), inBracket.end());
		itBracket = std::find(tokens.begin(), tokens.end(), "(");
		itEndBracket = std::find(tokens.begin(), tokens.end(), ")");
		tokens.erase(itBracket, itEndBracket + 1);

		itBracket = std::find(tokens.begin(), tokens.end(), "(");
		itEndBracket = std::find(tokens.begin(), tokens.end(), ")");
	}

	// Evaluate logical negations
	for (size_t j=0; (j+1)<tokens.size();) {
		if (setNegationOp.Contains(tokens[j][0])) {
			int isTrue = atoi(tokens[j+1].c_str());
			if (tokens[j] == "!")
				isTrue = !isTrue;
			std::vector<std::string>::iterator itInsert =
				tokens.erase(tokens.begin() + j, tokens.begin() + j + 2);
			tokens.insert(itInsert, isTrue ? "1" : "0");
		} else {
			j++;
		}
	}

	// Evaluate expressions in precedence order
	enum precedence { precArithmetic, precRelative, precLogical };
	for (int prec=precArithmetic; prec <= precLogical; prec++) {
		// Looking at 3 tokens at a time so end at 2 before end
		for (size_t k=0; (k+2)<tokens.size();) {
			char chOp = tokens[k+1][0];
			if (
				((prec==precArithmetic) && setArithmethicOp.Contains(chOp)) ||
				((prec==precRelative) && setRelOp.Contains(chOp)) ||
				((prec==precLogical) && setLogicalOp.Contains(chOp))
				) {
				int valA = atoi(tokens[k].c_str());
				int valB = atoi(tokens[k+2].c_str());
				int result = 0;
				if (tokens[k+1] == "+")
					result = valA + valB;
				else if (tokens[k+1] == "-")
					result = valA - valB;
				else if (tokens[k+1] == "*")
					result = valA * valB;
				else if (tokens[k+1] == "/")
					result = valA / (valB ? valB : 1);
				else if (tokens[k+1] == "%")
					result = valA % (valB ? valB : 1);
				else if (tokens[k+1] == "<")
					result = valA < valB;
				else if (tokens[k+1] == "<=")
					result = valA <= valB;
				else if (tokens[k+1] == ">")
					result = valA > valB;
				else if (tokens[k+1] == ">=")
					result = valA >= valB;
				else if (tokens[k+1] == "==")
					result = valA == valB;
				else if (tokens[k+1] == "!=")
					result = valA != valB;
				else if (tokens[k+1] == "||")
					result = valA || valB;
				else if (tokens[k+1] == "&&")
					result = valA && valB;
				char sResult[30];
				sprintf(sResult, "%d", result);
				std::vector<std::string>::iterator itInsert =
					tokens.erase(tokens.begin() + k, tokens.begin() + k + 3);
				tokens.insert(itInsert, sResult);
			} else {
				k++;
			}
		}
	}
}

bool LexerSourcePawn::EvaluateExpression(const std::string &expr, const std::map<std::string, std::string> &preprocessorDefinitions) {
	// Break into tokens, replacing with definitions
	std::string word;
	std::vector<std::string> tokens;
	const char *cp = expr.c_str();
	for (;;) {
		if (setWord.Contains(*cp)) {
			word += *cp;
		} else {
			std::map<std::string, std::string>::const_iterator it = preprocessorDefinitions.find(word);
			if (it != preprocessorDefinitions.end()) {
				tokens.push_back(it->second);
			} else if (!word.empty() && ((word[0] >= '0' && word[0] <= '9') || (word == "defined"))) {
				tokens.push_back(word);
			}
			word = "";
			if (!*cp) {
				break;
			}
			if ((*cp != ' ') && (*cp != '\t')) {
				std::string op(cp, 1);
				if (setRelOp.Contains(*cp)) {
					if (setRelOp.Contains(cp[1])) {
						op += cp[1];
						cp++;
					}
				} else if (setLogicalOp.Contains(*cp)) {
					if (setLogicalOp.Contains(cp[1])) {
						op += cp[1];
						cp++;
					}
				}
				tokens.push_back(op);
			}
		}
		cp++;
	}

	EvaluateTokens(tokens);

	// "0" or "" -> false else true
	bool isFalse = tokens.empty() ||
		((tokens.size() == 1) && ((tokens[0] == "") || tokens[0] == "0"));
	return !isFalse;
}

LexerModule lmSourcePawn(SCLEX_SOURCEPAWN, LexerSourcePawn::LexerFactorySourcePawn, "sourcepawn", sourcepawnWordLists);
