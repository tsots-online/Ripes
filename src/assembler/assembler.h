#pragma once

#include <QRegularExpression>

#include "assembler_defines.h"
#include "directive.h"
#include "instruction.h"
#include "isainfo.h"
#include "lexerutilities.h"
#include "matcher.h"
#include "pseudoinstruction.h"

#include <set>
#include <variant>

namespace Ripes {
namespace AssemblerTmp {

/**
 * A macro for running an assembler pass with error handling
 */
#define runPass(resName, resType, passFunction, ...)                             \
    auto passFunction##_res = passFunction(__VA_ARGS__);                         \
    try {                                                                        \
        auto& errors = std::get<Errors>(passFunction##_res);                     \
        result.errors.insert(result.errors.end(), errors.begin(), errors.end()); \
        return result;                                                           \
    } catch (const std::bad_variant_access&) {                                   \
    }                                                                            \
    auto resName = std::get<resType>(passFunction##_res);

/**
 * A macro for running an ISA-specific assembler instruction with error handling
 */
#define runOperation(resName, resType, operationFunction, ...)      \
    auto operationFunction##_res = operationFunction(__VA_ARGS__);  \
    try {                                                           \
        errors.push_back(std::get<Error>(operationFunction##_res)); \
        continue;                                                   \
    } catch (const std::bad_variant_access&) {                      \
    }                                                               \
    auto resName = std::get<resType>(operationFunction##_res);

template <typename ISA>
class AssemblerBase {
    static_assert(std::is_base_of<ISAInfoBase, ISA>::value, "Provided ISA type must derive from ISAInfoBase");

public:
    using Instr = Instruction<ISA>;
    using InstrMap = std::map<QString, std::shared_ptr<Instr>>;
    using InstrVec = std::vector<std::shared_ptr<Instr>>;
    using PseudoInstr = PseudoInstruction<ISA>;
    using PseudoInstrMap = std::map<QString, std::shared_ptr<PseudoInstr>>;
    using PseudoInstrVec = std::vector<std::shared_ptr<PseudoInstr>>;

private:
    struct LinkRequest {
        unsigned sourceLine;  // Source location of code which resulted in the link request
        uint32_t offset;      // Offset of instruction which needs link resolution

        // Reference to the immediate field which resolves the symbol and the requested symbol
        FieldLinkRequest fieldRequest;
    };
    using LinkRequests = std::vector<LinkRequest>;

public:
    AssembleResult assemble(const QString& program) const {
        const auto programLines = program.split(QRegExp("[\r\n]"));
        return assemble(programLines);
    }

    AssembleResult assemble(const QStringList& programLines) const {
        AssembleResult result;

        // Tokenize each source line and separate symbol from remainder of tokens
        runPass(tokenizedLines, Program, pass0, programLines);

        // Pseudo instruction expansion
        runPass(expandedLines, Program, pass1, tokenizedLines);

        /** Assemble. During assembly, we generate:
         * - symbolMap: Recoding the offset locations in the program of lines adorned with symbols
         * - linkageMap: Recording offsets of instructions which require linkage with symbols
         */
        SymbolMap symbolMap;
        LinkRequests needsLinkage;
        runPass(program, QByteArray, pass2, expandedLines, symbolMap, needsLinkage);

        // Symbol linkage
        runPass(unused, NoPassResult, pass3, program, symbolMap, needsLinkage);

        result.program = program;
        return result;
    }

    DisassembleResult disassemble(const QByteArray& program, const uint32_t baseAddress = 0) const {
        size_t i = 0;
        if (program.size() % sizeof(uint32_t) != 0) {
            throw std::runtime_error("Program instructions unaligned with instruction size");
        }
        DisassembleResult res;
        while (i < program.size()) {
            const uint32_t instructionWord = *reinterpret_cast<const uint32_t*>(program.data() + i);
            auto match = m_matcher->matchInstruction(instructionWord);
            try {
                auto& error = std::get<Error>(match);
                // Unknown instruction
                res.errors.push_back(error);
            } catch (const std::bad_variant_access&) {
                // Got match, disassemble
                auto tokens = std::get<const Instruction<ISA>*>(match)->disassemble(instructionWord, baseAddress + i,
                                                                                    ReverseSymbolMap());
                try {
                    auto& error = std::get<Error>(match);
                    // Error during disassembling
                    res.errors.push_back(error);
                } catch (const std::bad_variant_access&) {
                    res.program << std::get<LineTokens>(tokens).join(' ');
                }
            }
            i += sizeof(uint32_t);
        }
        return res;
    }

    const Matcher<ISA>& getMatcher() { return *m_matcher; }

private:
    /**
     * @brief pass0
     * Line tokenization and source line recording
     */
    std::variant<Errors, Program> pass0(const QStringList& program) const {
        Errors errors;
        Program tokenizedLines;
        tokenizedLines.reserve(program.size());

        /** @brief carry
         * A symbol should refer to the next following assembler line; whether an instruction or directive.
         * The carry is used to carry over symbol definitions from empty lines onto the next valid line.
         */
        Symbols carry;
        for (unsigned i = 0; i < program.size(); i++) {
            const auto& line = program.at(i);
            if (line.isEmpty())
                continue;
            TokenizedSrcLine tsl;
            tsl.sourceLine = i;
            runOperation(tokens, LineTokens, tokenize, program[i]);

            // Symbols precede directives
            runOperation(symbolsAndRest, SymbolLinePair, splitSymbolsFromLine, tokens, i);
            tsl.symbols = symbolsAndRest.first;

            runOperation(directivesAndRest, DirectivesLinePair, splitDirectivesFromLine, symbolsAndRest.second, i);
            tsl.directives = directivesAndRest.first;

            runOperation(remainingTokens, LineTokens, splitCommentFromLine, symbolsAndRest.second, i);
            tsl.tokens = remainingTokens;

            if (!tsl.symbols.empty() && tsl.tokens.empty()) {
                carry.insert(tsl.symbols.begin(), tsl.symbols.end());
            } else {
                if (!tsl.tokens.empty()) {
                    tsl.symbols.insert(carry.begin(), carry.end());
                    carry.clear();
                }
                tokenizedLines.push_back(tsl);
            }
        }
        if (errors.size() != 0) {
            return {errors};
        } else {
            return {tokenizedLines};
        }
    }

    /**
     * @brief pass1
     * Pseudo-op expansion. If @return errors is empty, pass succeeded.
     */
    std::variant<Errors, Program> pass1(const Program& tokenizedLines) const {
        Errors errors;
        Program expandedLines;
        expandedLines.reserve(tokenizedLines.size());

        for (unsigned i = 0; i < tokenizedLines.size(); i++) {
            const auto& tokenizedLine = tokenizedLines.at(i);
            runOperation(expandedOps, std::optional<std::vector<LineTokens>>, expandPseudoOp, tokenizedLine);
            if (expandedOps) {
                /** @note: Original source line is kept for all resulting lines after pseudo-op expantion.
                 * Labels and directives are only kept for the first expanded op.
                 */
                const auto& eops = expandedOps.value();
                for (int j = 0; j < eops.size(); j++) {
                    TokenizedSrcLine tsl;
                    tsl.tokens = eops.at(j);
                    tsl.sourceLine = tokenizedLine.sourceLine;
                    if (j == 0) {
                        tsl.directives = tokenizedLine.directives;
                        tsl.symbols = tokenizedLine.symbols;
                    }
                    expandedLines.push_back(tsl);
                }
            } else {
                // This was not a pseudoinstruction; just add line to the set of expanded lines
                expandedLines.push_back(tokenizedLine);
            }
        }

        if (errors.size() != 0) {
            return {errors};
        } else {
            return {expandedLines};
        }
    }

    /**
     * @brief pass2
     * Machine code translation. If @return errors is empty, pass succeeded.
     * In the following, current size of the program is used as an analog for the offset of the to-be-assembled
     * instruction in the program. This is then used for symbol resolution.
     */
    std::variant<Errors, QByteArray> pass2(const Program& tokenizedLines, SymbolMap& symbolMap,
                                           LinkRequests& needsLinkage) const {
        QByteArray program;
        Errors errors;
        for (const auto& line : tokenizedLines) {
            const uint32_t instr_offset = program.size();
            for (const auto& s : line.symbols) {
                if (symbolMap.count(s) != 0) {
                    errors.push_back(Error(line.sourceLine, "Multiple definitions of symbol '" + s + "'"));
                } else {
                    symbolMap[s] = instr_offset;
                }
            }

            runOperation(directiveBytes, std::optional<QByteArray>, assembleDirective, line);
            if (!directiveBytes) {
                std::weak_ptr<Instr> assembledWith;
                runOperation(machineCode, InstrRes, assembleInstruction, line, assembledWith);

                if (!machineCode.linksWithSymbol.symbol.isEmpty()) {
                    LinkRequest req;
                    req.sourceLine = line.sourceLine;
                    req.offset = instr_offset;
                    req.fieldRequest = machineCode.linksWithSymbol;
                    needsLinkage.push_back(req);
                }
                program.append(
                    QByteArray(reinterpret_cast<char*>(&machineCode.instruction), sizeof(machineCode.instruction)));

            } else {
                program.append(directiveBytes.value());
            }
        }
        if (errors.size() != 0) {
            return {errors};
        } else {
            return {program};
        }
    }

    std::variant<Errors, NoPassResult> pass3(QByteArray& program, const SymbolMap& symbolMap,
                                             const LinkRequests& needsLinkage) const {
        Errors errors;
        for (const auto& linkRequest : needsLinkage) {
            const auto& symbol = linkRequest.fieldRequest.symbol;
            if (symbolMap.count(symbol) == 0) {
                errors.push_back(Error(linkRequest.sourceLine, "Unknown symbol '" + symbol + "'"));
                continue;
            }

            // Decode instruction at link-request position
            assert(program.size() <= (linkRequest.offset + 4) &&
                   "Error: position of link request is not within program");
            uint32_t instr = *reinterpret_cast<uint32_t*>(program.data() + linkRequest.offset);

            // Re-apply immediate resolution using the value acquired from the symbol map
            if (auto* immField = dynamic_cast<const Imm*>(linkRequest.fieldRequest.field)) {
                immField->applySymbolResolution(symbolMap.at(symbol), instr, linkRequest.offset);
            } else {
                assert(false && "Something other than an immediate field has requested linkage?");
            }

            // Finally, overwrite the instruction in the program
            *reinterpret_cast<uint32_t*>(program.data() + linkRequest.offset) = instr;
        }
        if (errors.size() != 0) {
            return {errors};
        } else {
            return {NoPassResult()};
        }
    }

protected:
    AssemblerBase<ISA>() {}

    void initialize(InstrVec& instructions, PseudoInstrVec& pseudoinstructions, DirectiveVec& directives) {
        setInstructions(instructions);
        setPseudoInstructions(pseudoinstructions);
        setDirectives(directives);
        m_matcher = std::make_unique<Matcher<ISA>>(m_instructions);
    }

    void setDirectives(DirectiveVec& directives) {
        if (m_directives.size() != 0) {
            throw std::runtime_error("Directives already set");
        }
        m_directives = directives;
        for (const auto& iter : m_directives) {
            const auto directive = iter.get()->name();
            if (m_directivesMap.count(directive) != 0) {
                throw std::runtime_error("Error: directive " + directive.toStdString() +
                                         " has already been registerred.");
            }
            m_directivesMap[directive] = iter;
        }
    }

    void setPseudoInstructions(PseudoInstrVec& pseudoInstructions) {
        if (m_pseudoInstructions.size() != 0) {
            throw std::runtime_error("Pseudoinstructions already set");
        }
        m_pseudoInstructions = pseudoInstructions;

        for (const auto& iter : m_pseudoInstructions) {
            const auto instr_name = iter.get()->name();
            if (m_pseudoInstructionMap.count(instr_name) != 0) {
                throw std::runtime_error("Error: pseudo-instruction with opcode " + instr_name.toStdString() +
                                         " has already been registerred.");
            }
            m_pseudoInstructionMap[instr_name] = iter;
        }
    }

    void setInstructions(InstrVec& instructions) {
        if (m_instructions.size() != 0) {
            throw std::runtime_error("Instructions already set");
        }
        m_instructions = instructions;
        for (const auto& iter : m_instructions) {
            const auto instr_name = iter.get()->name();
            if (m_instructionMap.count(instr_name) != 0) {
                throw std::runtime_error("Error: instruction with opcode " + instr_name.toStdString() +
                                         " has already been registerred.");
            }
            m_instructionMap[instr_name] = iter;
        }
    }

    virtual PseudoExpandRes expandPseudoOp(const TokenizedSrcLine& line) const {
        if (line.tokens.empty()) {
            return std::optional<std::vector<AssemblerTmp::LineTokens>>{};
        }
        const auto& opcode = line.tokens.at(0);
        if (m_pseudoInstructionMap.count(opcode) == 0) {
            // Not a pseudo instruction
            return std::optional<std::vector<AssemblerTmp::LineTokens>>{};
        }
        return m_pseudoInstructionMap.at(opcode)->expand(line);
    }

    virtual std::variant<Error, LineTokens> tokenize(const QString& line) const {
        const static auto splitter = QRegularExpression(
            R"(\t|\((?=x(?:[1-2]\d|3[0-1]|\d)|t[0-6]|a[0-7]|s(?:1[0-1]|\d)|[sgt]p|zero)|(?:x(?:[1-2]\d|3[0-1]|\d)|t[0-6]|a[0-7]|s(?:1[0-1]|\d)|[sgt]p|zero)\K\))");
        return splitQuotes(line.split(splitter));
    }

    virtual AssembleRes assembleInstruction(const TokenizedSrcLine& line, std::weak_ptr<Instr>& assembledWith) const {
        if (line.tokens.empty()) {
            return {Error(line.sourceLine, "Empty source lines should be impossible at this point")};
        }
        const auto& opcode = line.tokens.at(0);
        if (m_instructionMap.count(opcode) == 0) {
            return {Error(line.sourceLine, "Unknown opcode '" + opcode + "'")};
        }
        assembledWith = m_instructionMap.at(opcode);
        return m_instructionMap.at(opcode)->assemble(line);
    };

    virtual HandleDirectiveRes assembleDirective(const TokenizedSrcLine& line) const {
        if (line.tokens.empty()) {
            return {Error(line.sourceLine, "Empty source lines should be impossible at this point")};
        }

        const auto& directive = line.tokens.at(0);
        if (m_directivesMap.count(directive) == 0) {
            // Not a directive
            return std::optional<QByteArray>();
        }
        return m_directivesMap.at(directive)->handle(line);
    };
    /**
     * @brief splitSymbolsFromLine
     * @returns a pair consisting of a symbol and the the input @p line tokens where the symbol has been removed.
     */
    virtual std::variant<Error, SymbolLinePair> splitSymbolsFromLine(const LineTokens& tokens, int sourceLine) const {
        if (tokens.size() == 0) {
            return std::pair<Symbols, LineTokens>({}, tokens);
        }

        LineTokens remainingTokens;
        remainingTokens.reserve(tokens.size());
        Symbols symbols;
        bool symbolStillAllowed = true;
        for (const auto& token : tokens) {
            if (token.contains(':')) {
                if (symbolStillAllowed) {
                    const QString cleanedSymbol = QString(token).remove(':');
                    if (symbols.count(cleanedSymbol) != 0) {
                        return Error(sourceLine, "Multiple definitions of symbol '" + cleanedSymbol + "'");
                    } else {
                        symbols.insert(cleanedSymbol);
                    }
                } else {
                    return Error(sourceLine, "Stray ':' in line");
                }
            } else {
                remainingTokens.append(token);
                symbolStillAllowed = false;
            }
        }
        return std::pair<Symbols, LineTokens>(symbols, remainingTokens);
    }

    virtual std::variant<Error, DirectivesLinePair> splitDirectivesFromLine(const LineTokens& tokens,
                                                                            int sourceLine) const {
        if (tokens.size() == 0) {
            return std::pair<Directives, LineTokens>({}, tokens);
        }

        LineTokens remainingTokens;
        remainingTokens.reserve(tokens.size());
        Directives directives;
        bool directivesStillAllowed = true;
        for (const auto& token : tokens) {
            if (token.startsWith('.')) {
                if (directivesStillAllowed) {
                    directives.insert(token);
                } else {
                    return Error(sourceLine, "Stray '.' in line");
                }
            } else {
                remainingTokens.append(token);
                directivesStillAllowed = false;
            }
        }
        return std::pair<Directives, LineTokens>(directives, remainingTokens);
    }

    virtual std::variant<Error, LineTokens> splitCommentFromLine(const LineTokens& tokens, int sourceLine) const {
        if (tokens.size() == 0) {
            return {tokens};
        }

        LineTokens preCommentTokens;
        preCommentTokens.reserve(tokens.size());
        for (const auto& token : tokens) {
            if (token.contains(commentDelimiter())) {
                break;
            } else {
                preCommentTokens.append(token);
            }
        }
        return {preCommentTokens};
    }

    virtual QString instrSegment() const { return QStringLiteral(".text"); }
    virtual QString dataSegment() const { return QStringLiteral(".data"); }
    virtual QChar commentDelimiter() const = 0;

    /**
     * @brief m_instructions is the set of instructions which can be matched from an instruction string as well as be
     * disassembled from a program.
     */
    InstrVec m_instructions;
    InstrMap m_instructionMap;

    /**
     * @brief m_pseudoInstructions is the set of instructions which can be matched from an instruction string but cannot
     * be disassembled from a program. Typically, pseudoinstructions will expand to one or more non-pseudo instructions.
     */
    PseudoInstrVec m_pseudoInstructions;
    PseudoInstrMap m_pseudoInstructionMap;

    /**
     * @brief m_assemblerDirectives is the set of supported assembler directives.
     */
    DirectiveVec m_directives;
    DirectiveMap m_directivesMap;

    /**
     * @brief m_segmentPointers maintains the current end-of-segment pointers for the segments annotated by the program
     * (ie. .text, .data, ...).
     */
    std::map<QString, uint32_t> m_segmentPointers;
    /**
     * @brief m_currentSegment maintains the current segment where the assembler emits information.
     */
    QString m_currentSegment;

    std::unique_ptr<Matcher<ISA>> m_matcher;
};

}  // namespace AssemblerTmp

}  // namespace Ripes