//===-- NaClObjDumpStream.cpp --------------------------------------------===//
//      Implements an objdump stream (bitcode records/assembly code).
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/Bitcode/NaCl/NaClObjDumpStream.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/ErrorHandling.h"

namespace llvm {
namespace naclbitc {

TextFormatter::TextFormatter(raw_ostream &BaseStream,
                             unsigned LineWidth,
                             const char *Tab)
    : TextIndenter(Tab),
      BaseStream(BaseStream),
      TextStream(TextBuffer),
      LineWidth(LineWidth),
      LinePosition(0),
      CurrentIndent(0),
      MinLineWidth(20),
      AtInstructionBeginning(true),
      LineIndent(),
      ContinuationIndent(),
      ClusteringLevel(0),
      ClusteredTextSize(0) {
  if (MinLineWidth > LineWidth) MinLineWidth = LineWidth;
}

TextFormatter::~TextFormatter() {}

void TextFormatter::WriteEndline() {
  assert(!IsClustering() && "Must close clustering before ending instruction");
  Write('\n');
  CurrentIndent = 0;
  AtInstructionBeginning = true;
  LineIndent.clear();
}

std::string TextFormatter::GetToken() {
  TextStream.flush();
  std::string Token(TextBuffer);
  TextBuffer.clear();
  if (!Token.empty() && IsClustering())
    AppendForReplay(GetTokenDirective::Allocate(this, Token));
  return Token;
}

void TextFormatter::Write(char ch) {
  switch (ch) {
  case '\n':
    BaseStream << ch;
    LinePosition = 0;
    break;
  case '\t': {
    size_t TabWidth = GetTabSize();
    size_t NumChars = LinePosition % TabWidth;
    if (NumChars == 0) NumChars = TabWidth;
    for (size_t i = 0; i < NumChars; ++i) Write(' ');
    break;
  }
  default:
    if (LinePosition == 0) {
      WriteLineIndents();
    }
    BaseStream << ch;
    ++LinePosition;
  }
  AtInstructionBeginning = false;
}

void TextFormatter::Write(const std::string &Text) {
  if (IsClustering()) {
    ClusteredTextSize += Text.size();
  } else {
    for (std::string::const_iterator
             Iter = Text.begin(), IterEnd = Text.end();
         Iter != IterEnd; ++Iter) {
      Write(*Iter);
    }
  }
}

void TextFormatter::StartClustering() {
  ++ClusteringLevel;
}

void TextFormatter::FinishClustering() {
  assert(IsClustering() && "Can't finish clustering, not in cluster!");
  --ClusteringLevel;
  if (IsClustering()) return;

  AddLineWrapIfNeeded(ClusteredTextSize);

  // Reapply the directives to generate the token text, and set
  // indentations. Because clustering can be nested, duplicate before
  // replaying, so that nested clusters can replayed and build its own
  // list of clustered directives.
  std::vector<const Directive*> Directives(ClusteredDirectives);
  ClusteredDirectives.clear();
  ClusteredTextSize = 0;
  for (std::vector<const Directive*>::iterator
           Iter = Directives.begin(),
           IterEnd = Directives.end();
       Iter != IterEnd; ++Iter) {
    (*Iter)->Apply();
  }
}

void TextFormatter::WriteLineIndents() {
  // Add line indent to base stream.
  if (AtInstructionBeginning) LineIndent = GetIndent();
  BaseStream << LineIndent;
  LinePosition += LineIndent.size();

  // If not the first line, and local indent not set, add continuation
  // indent to the base stream.
  unsigned UseIndent = CurrentIndent;
  if (!AtInstructionBeginning && CurrentIndent == 0) {
    UseIndent = FixIndentValue(LinePosition + ContinuationIndent.size());
  }

  // Add any additional indents local to the current instruction
  // being dumped to the base stream.
  for (; LinePosition < UseIndent; ++LinePosition) {
    BaseStream << ' ';
  }
}

void TextFormatter::Directive::Reapply() const {
  // Note: We don't want to store top-level start/finish cluster
  // directives on ClusteredDirectives, so that nested replays won't
  // reapply them.
  bool WasClustering = IsClustering();
  MyApply(true);
  if (WasClustering && IsClustering())
    Formatter->ClusteredDirectives.push_back(this);
}


TextFormatter::Directive *TextFormatter::GetTokenDirective::
Allocate(TextFormatter *Formatter, const std::string &Text) {
  GetTokenDirective *Dir = Formatter->GetTokenFreeList.Allocate(Formatter);
  Dir->Text = Text;
  return Dir;
}

std::string RecordTextFormatter::getBitAddress(uint64_t Bit) {
  std::string Address = naclbitc::getBitAddress(Bit);
  size_t AddressSize = Address.size();
  if (AddressSize >= AddressWriteWidth)
    return Address;

  // Pad address with leading spaces.
  std::string Buffer;
  raw_string_ostream StrBuf(Buffer);
  for (size_t i = AddressWriteWidth - AddressSize; i > 0; --i) {
    StrBuf << " ";
  }
  StrBuf << Address;
  return StrBuf.str();
}

RecordTextFormatter::RecordTextFormatter(ObjDumpStream *ObjDump)
    : TextFormatter(ObjDump->Records(), 0, "  "),
      OpenBrace(this, "<"),
      CloseBrace(this, ">"),
      Comma(this, ","),
      Space(this),
      Endline(this),
      StartCluster(this),
      FinishCluster(this) {
  // Handle fact that 64-bit values can take up to 21 characters.
  MinLineWidth = 21;
  Label = getBitAddress(0);
}

std::string RecordTextFormatter::GetEmptyLabelColumn() {
  std::string Buffer;
  raw_string_ostream StrmBuffer(Buffer);
  for (size_t i = 0; i < Label.size(); ++i) {
    StrmBuffer << ' ';
  }
  StrmBuffer << '|';
  return StrmBuffer.str();
}

void RecordTextFormatter::WriteLineIndents() {
  if (AtInstructionBeginning) {
    BaseStream << Label << '|';
  } else {
    BaseStream << GetEmptyLabelColumn();
  }
  LinePosition += Label.size() + 1;
  TextFormatter::WriteLineIndents();
}

void RecordTextFormatter::WriteValues(uint64_t Bit,
                                      const llvm::NaClBitcodeValues &Values,
                                      int32_t AbbrevIndex) {
  Label = getBitAddress(Bit);
  if (AbbrevIndex != ABBREV_INDEX_NOT_SPECIFIED) {
    TextStream << AbbrevIndex << ":" << Space;
  }
  TextStream << OpenBrace;
  for (size_t i = 0; i < Values.size(); ++i) {
    if (i > 0) {
      TextStream << Comma << FinishCluster << Space;
    }
    TextStream << StartCluster << Values[i];
  }
  // Note: Because of record codes, Values are never empty. Hence we
  // always need to finish the cluster for the last number printed.
  TextStream << FinishCluster << CloseBrace << Endline;
}

unsigned ObjDumpStream::DefaultMaxErrors = 20;

unsigned ObjDumpStream::ComboObjDumpSeparatorColumn = 40;

unsigned ObjDumpStream::RecordObjectDumpLength = 80;

ObjDumpStream::ObjDumpStream(raw_ostream &Stream,
                             bool DumpRecords, bool DumpAssembly)
    : Stream(Stream),
      DumpRecords(DumpRecords),
      DumpAssembly(DumpAssembly),
      NumErrors(0),
      MaxErrors(DefaultMaxErrors),
      RecordWidth(0),
      AssemblyBuffer(),
      AssemblyStream(AssemblyBuffer),
      MessageBuffer(),
      MessageStream(MessageBuffer),
      ColumnSeparator('|'),
      LastKnownBit(0),
      RecordBuffer(),
      RecordStream(RecordBuffer),
      RecordFormatter(this) {
  if (DumpRecords) {
    RecordWidth = DumpAssembly
        ? ComboObjDumpSeparatorColumn
        : RecordObjectDumpLength;
    RecordFormatter.SetLineWidth(RecordWidth);
  }
}

raw_ostream &ObjDumpStream::ErrorAt(naclbitc::ErrorLevel Level, uint64_t Bit) {
  // Note: Since this method only prints the error line prefix, and
  // lets the caller fill in the rest of the error.
  if (NumErrors >= MaxErrors) {
    // If we've hit the maximum number of errors, let Flush empty the buffers,
    // print out reported errors, and then exit the executable.
    Flush();
    llvm_unreachable("Flush shouldn't return if too many errors");
  }
  LastKnownBit = Bit;
  if (Level >= naclbitc::Error)
    ++NumErrors;
  return naclbitc::ErrorAt(Comments(), Level, Bit);
}

// Dumps the next line of text in the buffer. Returns the number of characters
// printed.
static size_t DumpLine(raw_ostream &Stream,
                       const std::string &Buffer,
                       size_t &Index,
                       size_t Size) {
  size_t Count = 0;
  while (Index < Size) {
    char ch = Buffer[Index];
    if (ch == '\n') {
      // At end of line, stop here.
      ++Index;
      return Count;
    } else {
      Stream << ch;
      ++Index;
      ++Count;
    }
  }
  return Count;
}

void ObjDumpStream::Flush() {
  // Start by flushing all buffers, so that we know the
  // text that must be written.
  RecordStream.flush();
  AssemblyStream.flush();
  MessageStream.flush();

  // See if there is any record/assembly lines to print.
  if ((DumpRecords && !RecordBuffer.empty())
      || (DumpAssembly && !AssemblyBuffer.empty())) {
    size_t RecordIndex = 0;
    size_t RecordSize = DumpRecords ? RecordBuffer.size() : 0;
    size_t AssemblyIndex = 0;
    size_t AssemblySize = DumpAssembly ? AssemblyBuffer.size() : 0;

    while (RecordIndex < RecordSize || AssemblyIndex < AssemblySize) {
      // Dump next record line.
      size_t Column = DumpLine(Stream, RecordBuffer, RecordIndex, RecordSize);
      // Now move to separator if assembly is to be printed also.
      if (DumpRecords && DumpAssembly) {
        if (Column == 0) {
          // Add indent filler.
          std::string Label = RecordFormatter.GetEmptyLabelColumn();
          Stream << Label;
          Column += Label.size();
        }
        for (size_t i = Column; i < RecordWidth; ++i) {
          Stream << ' ';
        }
        Stream << ColumnSeparator;
      }
      // Dump next assembly line.
      DumpLine(Stream, AssemblyBuffer, AssemblyIndex, AssemblySize);
      Stream << '\n';
    }
  }

  // Print out messages and reset buffers.
  Stream << MessageBuffer;
  Stream.flush();
  ResetBuffers();
  if (NumErrors >= MaxErrors)
    report_fatal_error("Too many errors, Unable to continue");
}

void ObjDumpStream::FlushThenQuit() {
  Flush();
  report_fatal_error("Unable to continue");
}

}
}
