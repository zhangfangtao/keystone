//===- lib/MC/MCFragment.cpp - Assembler Fragment Implementation ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCFragment.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCAsmLayout.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixupKindInfo.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"
#include <tuple>
using namespace llvm;

MCAsmLayout::MCAsmLayout(MCAssembler &Asm)
  : Assembler(Asm), LastValidFragment()
 {
  // Compute the section layout order. Virtual sections must go last.
  for (MCSection &Sec : Asm)
    if (!Sec.isVirtualSection())
      SectionOrder.push_back(&Sec);
  for (MCSection &Sec : Asm)
    if (Sec.isVirtualSection())
      SectionOrder.push_back(&Sec);
}

bool MCAsmLayout::isFragmentValid(const MCFragment *F) const {
  const MCSection *Sec = F->getParent();
  const MCFragment *LastValid = LastValidFragment.lookup(Sec);
  if (!LastValid)
    return false;
  assert(LastValid->getParent() == Sec);
  return F->getLayoutOrder() <= LastValid->getLayoutOrder();
}

void MCAsmLayout::invalidateFragmentsFrom(MCFragment *F) {
  // If this fragment wasn't already valid, we don't need to do anything.
  if (!isFragmentValid(F))
    return;

  // Otherwise, reset the last valid fragment to the previous fragment
  // (if this is the first fragment, it will be NULL).
  LastValidFragment[F->getParent()] = F->getPrevNode();
}

void MCAsmLayout::ensureValid(const MCFragment *F) const {
  MCSection *Sec = F->getParent();
  MCSection::iterator I;
  if (MCFragment *Cur = LastValidFragment[Sec])
    I = ++MCSection::iterator(Cur);
  else
    I = Sec->begin();

  // Advance the layout position until the fragment is valid.
  while (!isFragmentValid(F)) {
    assert(I != Sec->end() && "Layout bookkeeping error");
    const_cast<MCAsmLayout *>(this)->layoutFragment(&*I);
    ++I;
  }
}

uint64_t MCAsmLayout::getFragmentOffset(const MCFragment *F) const {
  ensureValid(F);
  assert(F->Offset != ~UINT64_C(0) && "Address not set!");
  return F->Offset;
}

// Simple getSymbolOffset helper for the non-varibale case.
static bool getLabelOffset(const MCAsmLayout &Layout, const MCSymbol &S,
                           bool ReportError, uint64_t &Val) {
  if (!S.getFragment()) {
    if (ReportError)
      report_fatal_error("unable to evaluate offset to undefined symbol '" +
                         S.getName() + "'");
    return false;
  }
  Val = Layout.getFragmentOffset(S.getFragment()) + S.getOffset();
  return true;
}

static bool getSymbolOffsetImpl(const MCAsmLayout &Layout, const MCSymbol &S,
                                bool ReportError, uint64_t &Val) {
  if (!S.isVariable())
    return getLabelOffset(Layout, S, ReportError, Val);

  // If SD is a variable, evaluate it.
  MCValue Target;
  if (!S.getVariableValue()->evaluateAsValue(Target, Layout))
    report_fatal_error("unable to evaluate offset for variable '" +
                       S.getName() + "'");

  uint64_t Offset = Target.getConstant();

  const MCSymbolRefExpr *A = Target.getSymA();
  if (A) {
    uint64_t ValA;
    if (!getLabelOffset(Layout, A->getSymbol(), ReportError, ValA))
      return false;
    Offset += ValA;
  }

  const MCSymbolRefExpr *B = Target.getSymB();
  if (B) {
    uint64_t ValB;
    if (!getLabelOffset(Layout, B->getSymbol(), ReportError, ValB))
      return false;
    Offset -= ValB;
  }

  Val = Offset;
  return true;
}

bool MCAsmLayout::getSymbolOffset(const MCSymbol &S, uint64_t &Val) const {
  return getSymbolOffsetImpl(*this, S, false, Val);
}

uint64_t MCAsmLayout::getSymbolOffset(const MCSymbol &S) const {
  uint64_t Val;
  getSymbolOffsetImpl(*this, S, true, Val);
  return Val;
}

const MCSymbol *MCAsmLayout::getBaseSymbol(const MCSymbol &Symbol) const {
  if (!Symbol.isVariable())
    return &Symbol;

  const MCExpr *Expr = Symbol.getVariableValue();
  MCValue Value;
  if (!Expr->evaluateAsValue(Value, *this)) {
    Assembler.getContext().reportError(
        SMLoc(), "expression could not be evaluated");
    return nullptr;
  }

  const MCSymbolRefExpr *RefB = Value.getSymB();
  if (RefB) {
    Assembler.getContext().reportError(
        SMLoc(), Twine("symbol '") + RefB->getSymbol().getName() +
                     "' could not be evaluated in a subtraction expression");
    return nullptr;
  }

  const MCSymbolRefExpr *A = Value.getSymA();
  if (!A)
    return nullptr;

  const MCSymbol &ASym = A->getSymbol();
  const MCAssembler &Asm = getAssembler();
  if (ASym.isCommon()) {
    // FIXME: we should probably add a SMLoc to MCExpr.
    Asm.getContext().reportError(SMLoc(),
                                 "Common symbol '" + ASym.getName() +
                                     "' cannot be used in assignment expr");
    return nullptr;
  }

  return &ASym;
}

uint64_t MCAsmLayout::getSectionAddressSize(const MCSection *Sec) const {
  // The size is the last fragment's end offset.
  const MCFragment &F = Sec->getFragmentList().back();
  bool valid;
  return getFragmentOffset(&F) + getAssembler().computeFragmentSize(*this, F, valid);
}

uint64_t MCAsmLayout::getSectionFileSize(const MCSection *Sec) const {
  // Virtual sections have no file size.
  if (Sec->isVirtualSection())
    return 0;

  // Otherwise, the file size is the same as the address space size.
  return getSectionAddressSize(Sec);
}

uint64_t llvm::computeBundlePadding(const MCAssembler &Assembler,
                                    const MCFragment *F,
                                    uint64_t FOffset, uint64_t FSize) {
  uint64_t BundleSize = Assembler.getBundleAlignSize();
  assert(BundleSize > 0 &&
         "computeBundlePadding should only be called if bundling is enabled");
  uint64_t BundleMask = BundleSize - 1;
  uint64_t OffsetInBundle = FOffset & BundleMask;
  uint64_t EndOfFragment = OffsetInBundle + FSize;

  // There are two kinds of bundling restrictions:
  //
  // 1) For alignToBundleEnd(), add padding to ensure that the fragment will
  //    *end* on a bundle boundary.
  // 2) Otherwise, check if the fragment would cross a bundle boundary. If it
  //    would, add padding until the end of the bundle so that the fragment
  //    will start in a new one.
  if (F->alignToBundleEnd()) {
    // Three possibilities here:
    //
    // A) The fragment just happens to end at a bundle boundary, so we're good.
    // B) The fragment ends before the current bundle boundary: pad it just
    //    enough to reach the boundary.
    // C) The fragment ends after the current bundle boundary: pad it until it
    //    reaches the end of the next bundle boundary.
    //
    // Note: this code could be made shorter with some modulo trickery, but it's
    // intentionally kept in its more explicit form for simplicity.
    if (EndOfFragment == BundleSize)
      return 0;
    else if (EndOfFragment < BundleSize)
      return BundleSize - EndOfFragment;
    else { // EndOfFragment > BundleSize
      return 2 * BundleSize - EndOfFragment;
    }
  } else if (OffsetInBundle > 0 && EndOfFragment > BundleSize)
    return BundleSize - OffsetInBundle;
  else
    return 0;
}

/* *** */

void ilist_node_traits<MCFragment>::deleteNode(MCFragment *V) {
  V->destroy();
}

MCFragment::MCFragment() : Kind(FragmentType(~0)), HasInstructions(false),
                           AlignToBundleEnd(false), BundlePadding(0) {
}

MCFragment::~MCFragment() { }

MCFragment::MCFragment(FragmentType Kind, bool HasInstructions,
                       uint8_t BundlePadding, MCSection *Parent)
    : Kind(Kind), HasInstructions(HasInstructions), AlignToBundleEnd(false),
      BundlePadding(BundlePadding), Parent(Parent), Atom(nullptr),
      Offset(~UINT64_C(0)) {
  if (Parent && !isDummy())
    Parent->getFragmentList().push_back(this);
}

void MCFragment::destroy() {
  // First check if we are the sentinal.
  if (Kind == FragmentType(~0)) {
    delete this;
    return;
  }

  switch (Kind) {
    case FT_Align:
      delete cast<MCAlignFragment>(this);
      return;
    case FT_Data:
      delete cast<MCDataFragment>(this);
      return;
    case FT_CompactEncodedInst:
      delete cast<MCCompactEncodedInstFragment>(this);
      return;
    case FT_Fill:
      delete cast<MCFillFragment>(this);
      return;
    case FT_Relaxable:
      delete cast<MCRelaxableFragment>(this);
      return;
    case FT_Org:
      delete cast<MCOrgFragment>(this);
      return;
    case FT_Dwarf:
      delete cast<MCDwarfLineAddrFragment>(this);
      return;
    case FT_DwarfFrame:
      delete cast<MCDwarfCallFrameFragment>(this);
      return;
    case FT_LEB:
      delete cast<MCLEBFragment>(this);
      return;
    case FT_SafeSEH:
      delete cast<MCSafeSEHFragment>(this);
      return;
    case FT_Dummy:
      delete cast<MCDummyFragment>(this);
      return;
  }
}

/* *** */

// Debugging methods

namespace llvm {

raw_ostream &operator<<(raw_ostream &OS, const MCFixup &AF) {
  OS << "<MCFixup" << " Offset:" << AF.getOffset()
     << " Value:" << *AF.getValue()
     << " Kind:" << AF.getKind() << ">";
  return OS;
}

}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void MCFragment::dump() {
  raw_ostream &OS = llvm::errs();

  OS << "<";
  switch (getKind()) {
  case MCFragment::FT_Align: OS << "MCAlignFragment"; break;
  case MCFragment::FT_Data:  OS << "MCDataFragment"; break;
  case MCFragment::FT_CompactEncodedInst:
    OS << "MCCompactEncodedInstFragment"; break;
  case MCFragment::FT_Fill:  OS << "MCFillFragment"; break;
  case MCFragment::FT_Relaxable:  OS << "MCRelaxableFragment"; break;
  case MCFragment::FT_Org:   OS << "MCOrgFragment"; break;
  case MCFragment::FT_Dwarf: OS << "MCDwarfFragment"; break;
  case MCFragment::FT_DwarfFrame: OS << "MCDwarfCallFrameFragment"; break;
  case MCFragment::FT_LEB:   OS << "MCLEBFragment"; break;
  case MCFragment::FT_SafeSEH:    OS << "MCSafeSEHFragment"; break;
  case MCFragment::FT_Dummy:
    OS << "MCDummyFragment";
    break;
  }

  OS << "<MCFragment " << (void*) this << " LayoutOrder:" << LayoutOrder
     << " Offset:" << Offset
     << " HasInstructions:" << hasInstructions() 
     << " BundlePadding:" << static_cast<unsigned>(getBundlePadding()) << ">";

  switch (getKind()) {
  case MCFragment::FT_Align: {
    const MCAlignFragment *AF = cast<MCAlignFragment>(this);
    if (AF->hasEmitNops())
      OS << " (emit nops)";
    OS << "\n       ";
    OS << " Alignment:" << AF->getAlignment()
       << " Value:" << AF->getValue() << " ValueSize:" << AF->getValueSize()
       << " MaxBytesToEmit:" << AF->getMaxBytesToEmit() << ">";
    break;
  }
  case MCFragment::FT_Data:  {
    const MCDataFragment *DF = cast<MCDataFragment>(this);
    OS << "\n       ";
    OS << " Contents:[";
    const SmallVectorImpl<char> &Contents = DF->getContents();
    for (unsigned i = 0, e = Contents.size(); i != e; ++i) {
      if (i) OS << ",";
      OS << hexdigit((Contents[i] >> 4) & 0xF) << hexdigit(Contents[i] & 0xF);
    }
    OS << "] (" << Contents.size() << " bytes)";

    if (DF->fixup_begin() != DF->fixup_end()) {
      OS << ",\n       ";
      OS << " Fixups:[";
      for (MCDataFragment::const_fixup_iterator it = DF->fixup_begin(),
             ie = DF->fixup_end(); it != ie; ++it) {
        if (it != DF->fixup_begin()) OS << ",\n                ";
        OS << *it;
      }
      OS << "]";
    }
    break;
  }
  case MCFragment::FT_CompactEncodedInst: {
    const MCCompactEncodedInstFragment *CEIF =
      cast<MCCompactEncodedInstFragment>(this);
    OS << "\n       ";
    OS << " Contents:[";
    const SmallVectorImpl<char> &Contents = CEIF->getContents();
    for (unsigned i = 0, e = Contents.size(); i != e; ++i) {
      if (i) OS << ",";
      OS << hexdigit((Contents[i] >> 4) & 0xF) << hexdigit(Contents[i] & 0xF);
    }
    OS << "] (" << Contents.size() << " bytes)";
    break;
  }
  case MCFragment::FT_Fill:  {
    const MCFillFragment *FF = cast<MCFillFragment>(this);
    OS << " Value:" << FF->getValue() << " Size:" << FF->getSize();
    break;
  }
  case MCFragment::FT_Relaxable:  {
    // const MCRelaxableFragment *F = cast<MCRelaxableFragment>(this);
    OS << "\n       ";
    OS << " Inst:";
    break;
  }
  case MCFragment::FT_Org:  {
    const MCOrgFragment *OF = cast<MCOrgFragment>(this);
    OS << "\n       ";
    OS << " Offset:" << OF->getOffset() << " Value:" << OF->getValue();
    break;
  }
  case MCFragment::FT_Dwarf:  {
    const MCDwarfLineAddrFragment *OF = cast<MCDwarfLineAddrFragment>(this);
    OS << "\n       ";
    OS << " AddrDelta:" << OF->getAddrDelta()
       << " LineDelta:" << OF->getLineDelta();
    break;
  }
  case MCFragment::FT_DwarfFrame:  {
    const MCDwarfCallFrameFragment *CF = cast<MCDwarfCallFrameFragment>(this);
    OS << "\n       ";
    OS << " AddrDelta:" << CF->getAddrDelta();
    break;
  }
  case MCFragment::FT_LEB: {
    const MCLEBFragment *LF = cast<MCLEBFragment>(this);
    OS << "\n       ";
    OS << " Value:" << LF->getValue() << " Signed:" << LF->isSigned();
    break;
  }
  case MCFragment::FT_SafeSEH: {
    const MCSafeSEHFragment *F = cast<MCSafeSEHFragment>(this);
    OS << "\n       ";
    OS << " Sym:" << F->getSymbol();
    break;
  }
  case MCFragment::FT_Dummy:
    break;
  }
  OS << ">";
}

LLVM_DUMP_METHOD void MCAssembler::dump() {
  raw_ostream &OS = llvm::errs();

  OS << "<MCAssembler\n";
  OS << "  Sections:[\n    ";
  for (iterator it = begin(), ie = end(); it != ie; ++it) {
    if (it != begin()) OS << ",\n    ";
    it->dump();
  }
  OS << "],\n";
  OS << "  Symbols:[";

  for (symbol_iterator it = symbol_begin(), ie = symbol_end(); it != ie; ++it) {
    if (it != symbol_begin()) OS << ",\n           ";
    OS << "(";
    it->dump();
    OS << ", Index:" << it->getIndex() << ", ";
    OS << ")";
  }
  OS << "]>\n";
}
#endif
