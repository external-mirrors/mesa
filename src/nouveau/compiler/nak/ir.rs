// Copyright © 2022 Collabora, Ltd.
// SPDX-License-Identifier: MIT

extern crate bitview;
extern crate nak_ir_proc;

use bitview::{BitMutView, BitMutViewable, BitView, BitViewable, SetField};
use nak_bindings::*;

pub use crate::builder::{Builder, InstrBuilder, SSABuilder, SSAInstrBuilder};
use crate::legalize::LegalizeBuilder;
use crate::sph::{OutputTopology, PixelImap};
pub use crate::ssa_value::*;
use compiler::as_slice::*;
use compiler::cfg::CFG;
use compiler::smallvec::SmallVec;
use nak_ir_proc::*;
use std::cmp::{max, min};
use std::fmt;
use std::fmt::Write;
use std::iter::Zip;
use std::ops::{BitAnd, BitOr, Deref, DerefMut, Index, IndexMut, Not, Range};
use std::slice;

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub struct Label {
    idx: u32,
}

impl fmt::Display for Label {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "L{}", self.idx)
    }
}

pub struct LabelAllocator {
    count: u32,
}

impl LabelAllocator {
    pub fn new() -> LabelAllocator {
        LabelAllocator { count: 0 }
    }

    pub fn alloc(&mut self) -> Label {
        let idx = self.count;
        self.count += 1;
        Label { idx: idx }
    }
}

/// Represents a register file
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum RegFile {
    /// The general-purpose register file
    ///
    /// General-purpose registers are 32 bits per SIMT channel.
    GPR = 0,

    /// The general-purpose uniform register file
    ///
    /// General-purpose uniform registers are 32 bits each and uniform across a
    /// wave.
    UGPR = 1,

    /// The predicate reigster file
    ///
    /// Predicate registers are 1 bit per SIMT channel.
    Pred = 2,

    /// The uniform predicate reigster file
    ///
    /// Uniform predicate registers are 1 bit and uniform across a wave.
    UPred = 3,

    /// The carry flag register file
    ///
    /// Only one carry flag register exists in hardware, but representing it as
    /// a reg file simplifies dependency tracking.
    ///
    /// This is used only on SM50.
    Carry = 4,

    /// The barrier register file
    ///
    /// This is a lane mask used for wave re-convergence instructions.
    Bar = 5,

    /// The memory register file
    ///
    /// This is a virtual register file for things which will get spilled to
    /// local memory.  Each memory location is 32 bits per SIMT channel.
    Mem = 6,
}

const NUM_REG_FILES: usize = 7;

impl RegFile {
    /// Returns true if the register file is uniform across a wave.
    pub fn is_uniform(&self) -> bool {
        match self {
            RegFile::GPR
            | RegFile::Pred
            | RegFile::Carry
            | RegFile::Bar
            | RegFile::Mem => false,
            RegFile::UGPR | RegFile::UPred => true,
        }
    }

    /// Returns the uniform form of this register file, if any.  For `GPR` and
    /// `UGPR, this returns `UGPR` and for `Pred` and `UPred`, this returns
    /// `UPred`.
    pub fn to_uniform(self) -> Option<RegFile> {
        match self {
            RegFile::GPR | RegFile::UGPR => Some(RegFile::UGPR),
            RegFile::Pred | RegFile::UPred => Some(RegFile::UPred),
            RegFile::Carry | RegFile::Bar | RegFile::Mem => None,
        }
    }

    /// Returns warp-wide version of this register file.
    pub fn to_warp(self) -> RegFile {
        match self {
            RegFile::GPR | RegFile::UGPR => RegFile::GPR,
            RegFile::Pred | RegFile::UPred => RegFile::Pred,
            RegFile::Carry | RegFile::Bar | RegFile::Mem => self,
        }
    }

    /// Returns true if the register file is GPR or UGPR.
    pub fn is_gpr(&self) -> bool {
        match self {
            RegFile::GPR | RegFile::UGPR => true,
            RegFile::Pred
            | RegFile::UPred
            | RegFile::Carry
            | RegFile::Bar
            | RegFile::Mem => false,
        }
    }

    /// Returns true if the register file is a predicate register file.
    pub fn is_predicate(&self) -> bool {
        match self {
            RegFile::GPR
            | RegFile::UGPR
            | RegFile::Carry
            | RegFile::Bar
            | RegFile::Mem => false,
            RegFile::Pred | RegFile::UPred => true,
        }
    }

    pub fn fmt_prefix(&self) -> &'static str {
        match self {
            RegFile::GPR => "r",
            RegFile::UGPR => "ur",
            RegFile::Pred => "p",
            RegFile::UPred => "up",
            RegFile::Carry => "c",
            RegFile::Bar => "b",
            RegFile::Mem => "m",
        }
    }
}

impl fmt::Display for RegFile {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            RegFile::GPR => write!(f, "GPR"),
            RegFile::UGPR => write!(f, "UGPR"),
            RegFile::Pred => write!(f, "Pred"),
            RegFile::UPred => write!(f, "UPred"),
            RegFile::Carry => write!(f, "Carry"),
            RegFile::Bar => write!(f, "Bar"),
            RegFile::Mem => write!(f, "Mem"),
        }
    }
}

impl From<RegFile> for u8 {
    fn from(value: RegFile) -> u8 {
        value as u8
    }
}

impl TryFrom<u32> for RegFile {
    type Error = &'static str;

    fn try_from(value: u32) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(RegFile::GPR),
            1 => Ok(RegFile::UGPR),
            2 => Ok(RegFile::Pred),
            3 => Ok(RegFile::UPred),
            4 => Ok(RegFile::Carry),
            5 => Ok(RegFile::Bar),
            6 => Ok(RegFile::Mem),
            _ => Err("Invalid register file number"),
        }
    }
}

impl TryFrom<u16> for RegFile {
    type Error = &'static str;

    fn try_from(value: u16) -> Result<Self, Self::Error> {
        RegFile::try_from(u32::from(value))
    }
}

impl TryFrom<u8> for RegFile {
    type Error = &'static str;

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        RegFile::try_from(u32::from(value))
    }
}

/// A trait for things which have an associated register file
pub trait HasRegFile {
    fn file(&self) -> RegFile;

    fn is_uniform(&self) -> bool {
        self.file().is_uniform()
    }

    fn is_gpr(&self) -> bool {
        self.file().is_gpr()
    }

    fn is_predicate(&self) -> bool {
        self.file().is_predicate()
    }
}

#[derive(Clone)]
pub struct RegFileSet {
    bits: u8,
}

impl RegFileSet {
    pub fn new() -> RegFileSet {
        RegFileSet { bits: 0 }
    }

    pub fn len(&self) -> usize {
        self.bits.count_ones() as usize
    }

    pub fn contains(&self, file: RegFile) -> bool {
        self.bits & (1 << (file as u8)) != 0
    }

    pub fn insert(&mut self, file: RegFile) -> bool {
        let has_file = self.contains(file);
        self.bits |= 1 << (file as u8);
        !has_file
    }

    pub fn is_empty(&self) -> bool {
        self.bits == 0
    }

    #[allow(dead_code)]
    pub fn iter(&self) -> RegFileSet {
        self.clone()
    }

    pub fn remove(&mut self, file: RegFile) -> bool {
        let has_file = self.contains(file);
        self.bits &= !(1 << (file as u8));
        has_file
    }
}

impl FromIterator<RegFile> for RegFileSet {
    fn from_iter<T: IntoIterator<Item = RegFile>>(iter: T) -> Self {
        let mut set = RegFileSet::new();
        for file in iter {
            set.insert(file);
        }
        set
    }
}

impl Iterator for RegFileSet {
    type Item = RegFile;

    fn next(&mut self) -> Option<RegFile> {
        if self.is_empty() {
            None
        } else {
            let file = self.bits.trailing_zeros().try_into().unwrap();
            self.remove(file);
            Some(file)
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let len = self.len();
        (len, Some(len))
    }
}

/// A container mapping register files to items.
///
/// This is used by several passes which need to replicate a data structure
/// per-register-file.
#[derive(Clone, Copy)]
pub struct PerRegFile<T> {
    per_file: [T; NUM_REG_FILES],
}

impl<T> PerRegFile<T> {
    /// Creates a new per-register-file container.
    ///
    /// Because this container assumes it always has an item for each register
    /// file, it takes a callback which maps register files to initial values
    /// to avoid adding a bunch of `Option<T>` or requiring `T` to implement
    /// `Default`.  If `T` does implement `Default`, then so does
    /// `PerRefFile<T>`.
    pub fn new_with<F: Fn(RegFile) -> T>(f: F) -> Self {
        PerRegFile {
            per_file: [
                f(RegFile::GPR),
                f(RegFile::UGPR),
                f(RegFile::Pred),
                f(RegFile::UPred),
                f(RegFile::Carry),
                f(RegFile::Bar),
                f(RegFile::Mem),
            ],
        }
    }

    /// Iterates over the values in this container.
    pub fn values(&self) -> slice::Iter<T> {
        self.per_file.iter()
    }

    /// Iterates over the mutable values in this container.
    pub fn values_mut(&mut self) -> slice::IterMut<T> {
        self.per_file.iter_mut()
    }
}

impl<T: Default> Default for PerRegFile<T> {
    fn default() -> Self {
        PerRegFile {
            per_file: Default::default(),
        }
    }
}

impl<T> Index<RegFile> for PerRegFile<T> {
    type Output = T;

    fn index(&self, idx: RegFile) -> &T {
        &self.per_file[idx as u8 as usize]
    }
}

impl<T> IndexMut<RegFile> for PerRegFile<T> {
    fn index_mut(&mut self, idx: RegFile) -> &mut T {
        &mut self.per_file[idx as u8 as usize]
    }
}

/// A reference to a contiguous range of registers in a particular register
/// file.
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub struct RegRef {
    packed: u32,
}

impl RegRef {
    pub const MAX_IDX: u32 = (1 << 26) - 1;

    /// Creates a new register reference.
    ///
    /// # Panics
    ///
    /// This method panics if `base_idx > RegRef::MAX_IDX` or if `comps > 8`.
    pub fn new(file: RegFile, base_idx: u32, comps: u8) -> RegRef {
        assert!(base_idx <= Self::MAX_IDX);
        let mut packed = base_idx;
        assert!(comps > 0 && comps <= 8);
        packed |= u32::from(comps - 1) << 26;
        assert!(u8::from(file) < 8);
        packed |= u32::from(u8::from(file)) << 29;
        RegRef { packed: packed }
    }

    /// Returns the index of the first register referenced.
    pub fn base_idx(&self) -> u32 {
        self.packed & 0x03ffffff
    }

    /// Returns the range of register indices referenced.
    pub fn idx_range(&self) -> Range<u32> {
        let start = self.base_idx();
        let end = start + u32::from(self.comps());
        start..end
    }

    /// Returns the number of registers referenced.
    pub fn comps(&self) -> u8 {
        (((self.packed >> 26) & 0x7) + 1).try_into().unwrap()
    }

    /// Returns a reference to the single register at `base_idx() + c`.
    pub fn comp(&self, c: u8) -> RegRef {
        assert!(c < self.comps());
        RegRef::new(self.file(), self.base_idx() + u32::from(c), 1)
    }
}

impl HasRegFile for RegRef {
    fn file(&self) -> RegFile {
        ((self.packed >> 29) & 0x7).try_into().unwrap()
    }
}

impl fmt::Display for RegRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}{}", self.file().fmt_prefix(), self.base_idx())?;
        if self.comps() > 1 {
            write!(f, "..{}", self.idx_range().end)?;
        }
        Ok(())
    }
}

#[derive(Clone)]
pub enum Dst {
    None,
    SSA(SSARef),
    Reg(RegRef),
}

impl Dst {
    pub fn is_none(&self) -> bool {
        matches!(self, Dst::None)
    }

    pub fn as_reg(&self) -> Option<&RegRef> {
        match self {
            Dst::Reg(r) => Some(r),
            _ => None,
        }
    }

    pub fn as_ssa(&self) -> Option<&SSARef> {
        match self {
            Dst::SSA(r) => Some(r),
            _ => None,
        }
    }

    #[allow(dead_code)]
    pub fn to_ssa(self) -> SSARef {
        match self {
            Dst::SSA(r) => r,
            _ => panic!("Expected ssa"),
        }
    }

    pub fn iter_ssa(&self) -> slice::Iter<'_, SSAValue> {
        match self {
            Dst::None | Dst::Reg(_) => &[],
            Dst::SSA(ssa) => ssa.deref(),
        }
        .iter()
    }

    pub fn iter_ssa_mut(&mut self) -> slice::IterMut<'_, SSAValue> {
        match self {
            Dst::None | Dst::Reg(_) => &mut [],
            Dst::SSA(ssa) => ssa.deref_mut(),
        }
        .iter_mut()
    }
}

impl From<RegRef> for Dst {
    fn from(reg: RegRef) -> Dst {
        Dst::Reg(reg)
    }
}

impl<T: Into<SSARef>> From<T> for Dst {
    fn from(ssa: T) -> Dst {
        Dst::SSA(ssa.into())
    }
}

impl From<Option<SSAValue>> for Dst {
    fn from(ssa: Option<SSAValue>) -> Dst {
        match ssa {
            Some(ssa) => Dst::SSA(ssa.into()),
            None => Dst::None,
        }
    }
}

impl fmt::Display for Dst {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Dst::None => write!(f, "null")?,
            Dst::SSA(v) => v.fmt(f)?,
            Dst::Reg(r) => r.fmt(f)?,
        }
        Ok(())
    }
}

#[derive(Clone, Eq, Hash, PartialEq)]
pub enum CBuf {
    Binding(u8),

    #[allow(dead_code)]
    BindlessSSA(SSARef),

    #[allow(dead_code)]
    BindlessUGPR(RegRef),
}

impl fmt::Display for CBuf {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CBuf::Binding(idx) => write!(f, "c[{:#x}]", idx),
            CBuf::BindlessSSA(v) => write!(f, "cx[{}]", v),
            CBuf::BindlessUGPR(r) => write!(f, "cx[{}]", r),
        }
    }
}

#[derive(Clone, Eq, Hash, PartialEq)]
pub struct CBufRef {
    pub buf: CBuf,
    pub offset: u16,
}

impl CBufRef {
    pub fn offset(self, offset: u16) -> CBufRef {
        CBufRef {
            buf: self.buf,
            offset: self.offset + offset,
        }
    }
}

impl fmt::Display for CBufRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}[{:#x}]", self.buf, self.offset)
    }
}

#[derive(Clone, Eq, Hash, PartialEq)]
pub enum SrcRef {
    Zero,
    True,
    False,
    Imm32(u32),
    CBuf(CBufRef),
    SSA(SSARef),
    Reg(RegRef),
}

impl SrcRef {
    #[allow(dead_code)]
    pub fn is_alu(&self) -> bool {
        match self {
            SrcRef::Zero | SrcRef::Imm32(_) | SrcRef::CBuf(_) => true,
            SrcRef::SSA(ssa) => ssa.is_gpr(),
            SrcRef::Reg(reg) => reg.is_gpr(),
            SrcRef::True | SrcRef::False => false,
        }
    }

    pub fn is_bindless_cbuf(&self) -> bool {
        match self {
            SrcRef::CBuf(cbuf) => {
                matches!(cbuf.buf, CBuf::BindlessSSA(_) | CBuf::BindlessUGPR(_))
            }
            _ => false,
        }
    }

    pub fn is_predicate(&self) -> bool {
        match self {
            SrcRef::Zero | SrcRef::Imm32(_) | SrcRef::CBuf(_) => false,
            SrcRef::True | SrcRef::False => true,
            SrcRef::SSA(ssa) => ssa.is_predicate(),
            SrcRef::Reg(reg) => reg.is_predicate(),
        }
    }

    pub fn is_carry(&self) -> bool {
        match self {
            SrcRef::SSA(ssa) => ssa.file() == Some(RegFile::Carry),
            SrcRef::Reg(reg) => reg.file() == RegFile::Carry,
            _ => false,
        }
    }

    #[allow(dead_code)]
    pub fn is_barrier(&self) -> bool {
        match self {
            SrcRef::SSA(ssa) => ssa.file() == Some(RegFile::Bar),
            SrcRef::Reg(reg) => reg.file() == RegFile::Bar,
            _ => false,
        }
    }

    pub fn as_reg(&self) -> Option<&RegRef> {
        match self {
            SrcRef::Reg(r) => Some(r),
            _ => None,
        }
    }

    pub fn as_ssa(&self) -> Option<&SSARef> {
        match self {
            SrcRef::SSA(r) => Some(r),
            _ => None,
        }
    }

    pub fn to_ssa(self) -> SSARef {
        match self {
            SrcRef::SSA(r) => r,
            _ => panic!(),
        }
    }

    pub fn as_u32(&self) -> Option<u32> {
        match self {
            SrcRef::Zero => Some(0),
            SrcRef::Imm32(u) => Some(*u),
            SrcRef::CBuf(_) | SrcRef::SSA(_) | SrcRef::Reg(_) => None,
            _ => panic!("Invalid integer source"),
        }
    }

    pub fn get_reg(&self) -> Option<&RegRef> {
        match self {
            SrcRef::Zero
            | SrcRef::True
            | SrcRef::False
            | SrcRef::Imm32(_)
            | SrcRef::SSA(_) => None,
            SrcRef::CBuf(cb) => match &cb.buf {
                CBuf::Binding(_) | CBuf::BindlessSSA(_) => None,
                CBuf::BindlessUGPR(reg) => Some(reg),
            },
            SrcRef::Reg(reg) => Some(reg),
        }
    }

    pub fn iter_ssa(&self) -> slice::Iter<'_, SSAValue> {
        match self {
            SrcRef::Zero
            | SrcRef::True
            | SrcRef::False
            | SrcRef::Imm32(_)
            | SrcRef::Reg(_) => &[],
            SrcRef::CBuf(cb) => match &cb.buf {
                CBuf::Binding(_) | CBuf::BindlessUGPR(_) => &[],
                CBuf::BindlessSSA(ssa) => ssa.deref(),
            },
            SrcRef::SSA(ssa) => ssa.deref(),
        }
        .iter()
    }

    pub fn iter_ssa_mut(&mut self) -> slice::IterMut<'_, SSAValue> {
        match self {
            SrcRef::Zero
            | SrcRef::True
            | SrcRef::False
            | SrcRef::Imm32(_)
            | SrcRef::Reg(_) => &mut [],
            SrcRef::CBuf(cb) => match &mut cb.buf {
                CBuf::Binding(_) | CBuf::BindlessUGPR(_) => &mut [],
                CBuf::BindlessSSA(ssa) => ssa.deref_mut(),
            },
            SrcRef::SSA(ssa) => ssa.deref_mut(),
        }
        .iter_mut()
    }
}

impl From<bool> for SrcRef {
    fn from(b: bool) -> SrcRef {
        if b {
            SrcRef::True
        } else {
            SrcRef::False
        }
    }
}

impl From<u32> for SrcRef {
    fn from(u: u32) -> SrcRef {
        if u == 0 {
            SrcRef::Zero
        } else {
            SrcRef::Imm32(u)
        }
    }
}

impl From<f32> for SrcRef {
    fn from(f: f32) -> SrcRef {
        f.to_bits().into()
    }
}

impl From<PrmtSel> for SrcRef {
    fn from(sel: PrmtSel) -> SrcRef {
        u32::from(sel.0).into()
    }
}

impl From<CBufRef> for SrcRef {
    fn from(cb: CBufRef) -> SrcRef {
        SrcRef::CBuf(cb)
    }
}

impl From<RegRef> for SrcRef {
    fn from(reg: RegRef) -> SrcRef {
        SrcRef::Reg(reg)
    }
}

impl<T: Into<SSARef>> From<T> for SrcRef {
    fn from(ssa: T) -> SrcRef {
        SrcRef::SSA(ssa.into())
    }
}

impl fmt::Display for SrcRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            SrcRef::Zero => write!(f, "rZ"),
            SrcRef::True => write!(f, "pT"),
            SrcRef::False => write!(f, "pF"),
            SrcRef::Imm32(u) => write!(f, "{:#x}", u),
            SrcRef::CBuf(c) => c.fmt(f),
            SrcRef::SSA(v) => v.fmt(f),
            SrcRef::Reg(r) => r.fmt(f),
        }
    }
}

#[derive(Clone, Copy, PartialEq)]
pub enum SrcMod {
    None,
    FAbs,
    FNeg,
    FNegAbs,
    INeg,
    BNot,
}

impl SrcMod {
    pub fn is_none(&self) -> bool {
        matches!(self, SrcMod::None)
    }

    pub fn has_fabs(&self) -> bool {
        match self {
            SrcMod::None | SrcMod::FNeg => false,
            SrcMod::FAbs | SrcMod::FNegAbs => true,
            _ => panic!("Not a float modifier"),
        }
    }

    pub fn has_fneg(&self) -> bool {
        match self {
            SrcMod::None | SrcMod::FAbs => false,
            SrcMod::FNeg | SrcMod::FNegAbs => true,
            _ => panic!("Not a float modifier"),
        }
    }

    pub fn is_ineg(&self) -> bool {
        match self {
            SrcMod::None => false,
            SrcMod::INeg => true,
            _ => panic!("Not an integer modifier"),
        }
    }

    pub fn is_bnot(&self) -> bool {
        match self {
            SrcMod::None => false,
            SrcMod::BNot => true,
            _ => panic!("Not a bitwise modifier"),
        }
    }

    pub fn fabs(self) -> SrcMod {
        match self {
            SrcMod::None | SrcMod::FAbs | SrcMod::FNeg | SrcMod::FNegAbs => {
                SrcMod::FAbs
            }
            _ => panic!("Not a float source modifier"),
        }
    }

    pub fn fneg(self) -> SrcMod {
        match self {
            SrcMod::None => SrcMod::FNeg,
            SrcMod::FAbs => SrcMod::FNegAbs,
            SrcMod::FNeg => SrcMod::None,
            SrcMod::FNegAbs => SrcMod::FAbs,
            _ => panic!("Not a float source modifier"),
        }
    }

    pub fn ineg(self) -> SrcMod {
        match self {
            SrcMod::None => SrcMod::INeg,
            SrcMod::INeg => SrcMod::None,
            _ => panic!("Not an integer source modifier"),
        }
    }

    pub fn bnot(self) -> SrcMod {
        match self {
            SrcMod::None => SrcMod::BNot,
            SrcMod::BNot => SrcMod::None,
            _ => panic!("Not a boolean source modifier"),
        }
    }

    pub fn modify(self, other: SrcMod) -> SrcMod {
        match other {
            SrcMod::None => self,
            SrcMod::FAbs => self.fabs(),
            SrcMod::FNeg => self.fneg(),
            SrcMod::FNegAbs => self.fabs().fneg(),
            SrcMod::INeg => self.ineg(),
            SrcMod::BNot => self.bnot(),
        }
    }
}

#[derive(Clone, Copy, PartialEq)]
#[allow(dead_code)]
pub enum SrcSwizzle {
    None,
    Xx,
    Yy,
}

impl SrcSwizzle {
    pub fn is_none(&self) -> bool {
        matches!(self, SrcSwizzle::None)
    }
}

impl fmt::Display for SrcSwizzle {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            SrcSwizzle::None => Ok(()),
            SrcSwizzle::Xx => write!(f, ".xx"),
            SrcSwizzle::Yy => write!(f, ".yy"),
        }
    }
}

#[derive(Clone, PartialEq)]
pub struct Src {
    pub src_ref: SrcRef,
    pub src_mod: SrcMod,
    pub src_swizzle: SrcSwizzle,
}

impl Src {
    pub const ZERO: Src = Src {
        src_ref: SrcRef::Zero,
        src_mod: SrcMod::None,
        src_swizzle: SrcSwizzle::None,
    };

    pub fn new_imm_u32(u: u32) -> Src {
        u.into()
    }

    pub fn new_imm_bool(b: bool) -> Src {
        b.into()
    }

    pub fn is_unmodified(&self) -> bool {
        self.src_mod.is_none() && self.src_swizzle.is_none()
    }

    pub fn fabs(self) -> Src {
        Src {
            src_ref: self.src_ref,
            src_mod: self.src_mod.fabs(),
            src_swizzle: self.src_swizzle,
        }
    }

    pub fn fneg(self) -> Src {
        Src {
            src_ref: self.src_ref,
            src_mod: self.src_mod.fneg(),
            src_swizzle: self.src_swizzle,
        }
    }

    pub fn ineg(self) -> Src {
        Src {
            src_ref: self.src_ref,
            src_mod: self.src_mod.ineg(),
            src_swizzle: self.src_swizzle,
        }
    }

    pub fn bnot(self) -> Src {
        Src {
            src_ref: self.src_ref,
            src_mod: self.src_mod.bnot(),
            src_swizzle: self.src_swizzle,
        }
    }

    pub fn modify(mut self, src_mod: SrcMod) -> Src {
        self.src_mod = self.src_mod.modify(src_mod);
        self
    }

    pub fn as_u32(&self, src_type: SrcType) -> Option<u32> {
        let u = match &self.src_ref {
            SrcRef::Zero => 0,
            SrcRef::Imm32(u) => *u,
            _ => return None,
        };

        if self.is_unmodified() {
            return Some(u);
        }

        assert!(src_type == SrcType::F16v2 || self.src_swizzle.is_none());

        // INeg affects more than just the 32 bits of input data so it can't be
        // trivially folded.  In fact, -imm may not be representable as a 32-bit
        // immediate at all.
        if src_type == SrcType::I32 {
            return None;
        }

        Some(match src_type {
            SrcType::F16 => {
                let low = u & 0xFFFF;

                match self.src_mod {
                    SrcMod::None => low,
                    SrcMod::FAbs => low & !(1_u32 << 15),
                    SrcMod::FNeg => low ^ (1_u32 << 15),
                    SrcMod::FNegAbs => low | (1_u32 << 15),
                    _ => panic!("Not a float source modifier"),
                }
            }
            SrcType::F16v2 => {
                let u = match self.src_swizzle {
                    SrcSwizzle::None => u,
                    SrcSwizzle::Xx => (u << 16) | (u & 0xffff),
                    SrcSwizzle::Yy => (u & 0xffff0000) | (u >> 16),
                };

                match self.src_mod {
                    SrcMod::None => u,
                    SrcMod::FAbs => u & 0x7FFF7FFF,
                    SrcMod::FNeg => u ^ 0x80008000,
                    SrcMod::FNegAbs => u | 0x80008000,
                    _ => panic!("Not a float source modifier"),
                }
            }
            SrcType::F32 | SrcType::F64 => match self.src_mod {
                SrcMod::None => u,
                SrcMod::FAbs => u & !(1_u32 << 31),
                SrcMod::FNeg => u ^ (1_u32 << 31),
                SrcMod::FNegAbs => u | (1_u32 << 31),
                _ => panic!("Not a float source modifier"),
            },
            SrcType::I32 => match self.src_mod {
                SrcMod::None => u,
                SrcMod::INeg => -(u as i32) as u32,
                _ => panic!("Not an integer source modifier"),
            },
            SrcType::B32 => match self.src_mod {
                SrcMod::None => u,
                SrcMod::BNot => !u,
                _ => panic!("Not a bitwise source modifier"),
            },
            _ => {
                assert!(self.is_unmodified());
                u
            }
        })
    }

    pub fn as_ssa(&self) -> Option<&SSARef> {
        if self.is_unmodified() {
            self.src_ref.as_ssa()
        } else {
            None
        }
    }

    pub fn to_ssa(self) -> SSARef {
        if self.is_unmodified() {
            self.src_ref.to_ssa()
        } else {
            panic!("Did not expect src_mod");
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match &self.src_ref {
            SrcRef::True => Some(!self.src_mod.is_bnot()),
            SrcRef::False => Some(self.src_mod.is_bnot()),
            SrcRef::SSA(vec) => {
                assert!(vec.is_predicate() && vec.comps() == 1);
                None
            }
            SrcRef::Reg(reg) => {
                assert!(reg.is_predicate() && reg.comps() == 1);
                None
            }
            _ => panic!("Not a boolean source"),
        }
    }

    pub fn as_imm_not_i20(&self) -> Option<u32> {
        match self.src_ref {
            SrcRef::Imm32(i) => {
                assert!(self.is_unmodified());
                let top = i & 0xfff80000;
                if top == 0 || top == 0xfff80000 {
                    None
                } else {
                    Some(i)
                }
            }
            _ => None,
        }
    }

    pub fn as_imm_not_f20(&self) -> Option<u32> {
        match self.src_ref {
            SrcRef::Imm32(i) => {
                assert!(self.is_unmodified());
                if (i & 0xfff) == 0 {
                    None
                } else {
                    Some(i)
                }
            }
            _ => None,
        }
    }

    pub fn iter_ssa(&self) -> slice::Iter<'_, SSAValue> {
        self.src_ref.iter_ssa()
    }

    pub fn iter_ssa_mut(&mut self) -> slice::IterMut<'_, SSAValue> {
        self.src_ref.iter_ssa_mut()
    }

    pub fn is_uniform(&self) -> bool {
        match &self.src_ref {
            SrcRef::Zero
            | SrcRef::True
            | SrcRef::False
            | SrcRef::Imm32(_)
            | SrcRef::CBuf(_) => true,
            SrcRef::SSA(ssa) => ssa.is_uniform(),
            SrcRef::Reg(reg) => reg.is_uniform(),
        }
    }

    pub fn is_bindless_cbuf(&self) -> bool {
        self.src_ref.is_bindless_cbuf()
    }

    pub fn is_predicate(&self) -> bool {
        self.src_ref.is_predicate()
    }

    pub fn is_zero(&self) -> bool {
        match self.src_ref {
            SrcRef::Zero | SrcRef::Imm32(0) => match self.src_mod {
                SrcMod::None | SrcMod::FAbs => true,
                SrcMod::FNeg | SrcMod::FNegAbs | SrcMod::BNot => false,
                // INeg affects more than just the 32 bits of input data so -0
                // may not be equivalent to 0.
                SrcMod::INeg => false,
            },
            _ => false,
        }
    }

    pub fn is_nonzero(&self) -> bool {
        assert!(self.is_unmodified());
        matches!(self.src_ref, SrcRef::Imm32(x) if x != 0)
    }

    pub fn is_fneg_zero(&self, src_type: SrcType) -> bool {
        match self.as_u32(src_type) {
            Some(0x00008000) => src_type == SrcType::F16,
            Some(0x80000000) => {
                src_type == SrcType::F32 || src_type == SrcType::F64
            }
            Some(0x80008000) => src_type == SrcType::F16v2,
            _ => false,
        }
    }

    #[allow(dead_code)]
    pub fn supports_type(&self, src_type: &SrcType) -> bool {
        match src_type {
            SrcType::SSA => {
                if !self.is_unmodified() {
                    return false;
                }

                matches!(self.src_ref, SrcRef::SSA(_) | SrcRef::Reg(_))
            }
            SrcType::GPR => {
                if !self.is_unmodified() {
                    return false;
                }

                matches!(
                    self.src_ref,
                    SrcRef::Zero | SrcRef::SSA(_) | SrcRef::Reg(_)
                )
            }
            SrcType::ALU => self.is_unmodified() && self.src_ref.is_alu(),
            SrcType::F16 | SrcType::F32 | SrcType::F64 | SrcType::F16v2 => {
                match self.src_mod {
                    SrcMod::None
                    | SrcMod::FAbs
                    | SrcMod::FNeg
                    | SrcMod::FNegAbs => (),
                    _ => return false,
                }

                self.src_ref.is_alu()
            }
            SrcType::I32 => {
                match self.src_mod {
                    SrcMod::None | SrcMod::INeg => (),
                    _ => return false,
                }

                self.src_ref.is_alu()
            }
            SrcType::B32 => {
                match self.src_mod {
                    SrcMod::None | SrcMod::BNot => (),
                    _ => return false,
                }

                self.src_ref.is_alu()
            }
            SrcType::Pred => {
                match self.src_mod {
                    SrcMod::None | SrcMod::BNot => (),
                    _ => return false,
                }

                self.src_ref.is_predicate()
            }
            SrcType::Carry => self.is_unmodified() && self.src_ref.is_carry(),
            SrcType::Bar => self.is_unmodified() && self.src_ref.is_barrier(),
        }
    }
}

impl<T: Into<SrcRef>> From<T> for Src {
    fn from(value: T) -> Src {
        Src {
            src_ref: value.into(),
            src_mod: SrcMod::None,
            src_swizzle: SrcSwizzle::None,
        }
    }
}

impl fmt::Display for Src {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.src_mod {
            SrcMod::None => write!(f, "{}{}", self.src_ref, self.src_swizzle),
            SrcMod::FAbs => write!(f, "|{}{}|", self.src_ref, self.src_swizzle),
            SrcMod::FNeg => write!(f, "-{}{}", self.src_ref, self.src_swizzle),
            SrcMod::FNegAbs => {
                write!(f, "-|{}{}|", self.src_ref, self.src_swizzle)
            }
            SrcMod::INeg => write!(f, "-{}{}", self.src_ref, self.src_swizzle),
            SrcMod::BNot => write!(f, "!{}{}", self.src_ref, self.src_swizzle),
        }
    }
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum SrcType {
    SSA,
    GPR,
    ALU,
    F16,
    F16v2,
    F32,
    F64,
    I32,
    B32,
    Pred,
    Carry,
    Bar,
}

impl SrcType {
    const DEFAULT: SrcType = SrcType::GPR;
}

pub type SrcTypeList = AttrList<SrcType>;

pub trait SrcsAsSlice: AsSlice<Src, Attr = SrcType> {
    fn srcs_as_slice(&self) -> &[Src] {
        self.as_slice()
    }

    fn srcs_as_mut_slice(&mut self) -> &mut [Src] {
        self.as_mut_slice()
    }

    fn src_types(&self) -> SrcTypeList {
        self.attrs()
    }

    fn src_idx(&self, src: &Src) -> usize {
        let r = self.srcs_as_slice().as_ptr_range();
        assert!(r.contains(&(src as *const Src)));
        unsafe { (src as *const Src).offset_from(r.start) as usize }
    }
}

impl<T: AsSlice<Src, Attr = SrcType>> SrcsAsSlice for T {}

fn all_dsts_uniform(dsts: &[Dst]) -> bool {
    let mut uniform = None;
    for dst in dsts {
        let dst_uniform = match dst {
            Dst::None => continue,
            Dst::Reg(r) => r.is_uniform(),
            Dst::SSA(r) => r.file().unwrap().is_uniform(),
        };
        assert!(uniform.is_none() || uniform == Some(dst_uniform));
        uniform = Some(dst_uniform);
    }
    uniform == Some(true)
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum DstType {
    Pred,
    GPR,
    F16,
    F16v2,
    F32,
    F64,
    Carry,
    Bar,
    Vec,
}

impl DstType {
    const DEFAULT: DstType = DstType::Vec;
}

pub type DstTypeList = AttrList<DstType>;

pub trait DstsAsSlice: AsSlice<Dst, Attr = DstType> {
    fn dsts_as_slice(&self) -> &[Dst] {
        self.as_slice()
    }

    fn dsts_as_mut_slice(&mut self) -> &mut [Dst] {
        self.as_mut_slice()
    }

    // Currently only used by test code
    #[allow(dead_code)]
    fn dst_types(&self) -> DstTypeList {
        self.attrs()
    }

    fn dst_idx(&self, dst: &Dst) -> usize {
        let r = self.dsts_as_slice().as_ptr_range();
        assert!(r.contains(&(dst as *const Dst)));
        unsafe { (dst as *const Dst).offset_from(r.start) as usize }
    }
}

impl<T: AsSlice<Dst, Attr = DstType>> DstsAsSlice for T {}

pub trait IsUniform {
    fn is_uniform(&self) -> bool;
}

impl<T: DstsAsSlice> IsUniform for T {
    fn is_uniform(&self) -> bool {
        all_dsts_uniform(self.dsts_as_slice())
    }
}

fn fmt_dst_slice(f: &mut fmt::Formatter<'_>, dsts: &[Dst]) -> fmt::Result {
    if dsts.is_empty() {
        return Ok(());
    }

    // Figure out the last non-null dst
    //
    // Note: By making the top inclusive and starting at 0, we ensure that
    // at least one dst always gets printed.
    let mut last_dst = 0;
    for (i, dst) in dsts.iter().enumerate() {
        if !dst.is_none() {
            last_dst = i;
        }
    }

    for i in 0..(last_dst + 1) {
        if i != 0 {
            write!(f, " ")?;
        }
        write!(f, "{}", &dsts[i])?;
    }
    Ok(())
}

#[allow(dead_code)]
#[derive(Clone, Copy)]
pub enum FoldData {
    Pred(bool),
    Carry(bool),
    U32(u32),
    Vec2([u32; 2]),
}

pub struct OpFoldData<'a> {
    pub dsts: &'a mut [FoldData],
    pub srcs: &'a [FoldData],
}

impl OpFoldData<'_> {
    pub fn get_pred_src(&self, op: &impl SrcsAsSlice, src: &Src) -> bool {
        let i = op.src_idx(src);
        let b = match src.src_ref {
            SrcRef::Zero | SrcRef::Imm32(_) => panic!("Expected a predicate"),
            SrcRef::True => true,
            SrcRef::False => false,
            _ => {
                if let FoldData::Pred(b) = self.srcs[i] {
                    b
                } else {
                    panic!("FoldData is not a predicate");
                }
            }
        };
        b ^ src.src_mod.is_bnot()
    }

    pub fn get_u32_src(&self, op: &impl SrcsAsSlice, src: &Src) -> u32 {
        let i = op.src_idx(src);
        match src.src_ref {
            SrcRef::Zero => 0,
            SrcRef::Imm32(imm) => imm,
            SrcRef::True | SrcRef::False => panic!("Unexpected predicate"),
            _ => {
                if let FoldData::U32(u) = self.srcs[i] {
                    u
                } else {
                    panic!("FoldData is not a U32");
                }
            }
        }
    }

    pub fn get_u32_bnot_src(&self, op: &impl SrcsAsSlice, src: &Src) -> u32 {
        let x = self.get_u32_src(op, src);
        if src.src_mod.is_bnot() {
            !x
        } else {
            x
        }
    }

    pub fn get_carry_src(&self, op: &impl SrcsAsSlice, src: &Src) -> bool {
        assert!(src.src_ref.as_ssa().is_some());
        let i = op.src_idx(src);
        if let FoldData::Carry(b) = self.srcs[i] {
            b
        } else {
            panic!("FoldData is not a predicate");
        }
    }

    #[allow(dead_code)]
    pub fn get_f32_src(&self, op: &impl SrcsAsSlice, src: &Src) -> f32 {
        f32::from_bits(self.get_u32_src(op, src))
    }

    #[allow(dead_code)]
    pub fn get_f64_src(&self, op: &impl SrcsAsSlice, src: &Src) -> f64 {
        let i = op.src_idx(src);
        match src.src_ref {
            SrcRef::Zero => 0.0,
            SrcRef::Imm32(imm) => f64::from_bits(u64::from(imm) << 32),
            SrcRef::True | SrcRef::False => panic!("Unexpected predicate"),
            _ => {
                if let FoldData::Vec2(v) = self.srcs[i] {
                    let u = u64::from(v[0]) | (u64::from(v[1]) << 32);
                    f64::from_bits(u)
                } else {
                    panic!("FoldData is not a U32");
                }
            }
        }
    }

    pub fn set_pred_dst(&mut self, op: &impl DstsAsSlice, dst: &Dst, b: bool) {
        self.dsts[op.dst_idx(dst)] = FoldData::Pred(b);
    }

    pub fn set_carry_dst(&mut self, op: &impl DstsAsSlice, dst: &Dst, b: bool) {
        self.dsts[op.dst_idx(dst)] = FoldData::Carry(b);
    }

    pub fn set_u32_dst(&mut self, op: &impl DstsAsSlice, dst: &Dst, u: u32) {
        self.dsts[op.dst_idx(dst)] = FoldData::U32(u);
    }

    #[allow(dead_code)]
    pub fn set_f32_dst(&mut self, op: &impl DstsAsSlice, dst: &Dst, f: f32) {
        self.set_u32_dst(op, dst, f.to_bits());
    }

    #[allow(dead_code)]
    pub fn set_f64_dst(&mut self, op: &impl DstsAsSlice, dst: &Dst, f: f64) {
        let u = f.to_bits();
        let v = [u as u32, (u >> 32) as u32];
        self.dsts[op.dst_idx(dst)] = FoldData::Vec2(v);
    }
}

pub trait Foldable: SrcsAsSlice + DstsAsSlice {
    // Currently only used by test code
    #[allow(dead_code)]
    fn fold(&self, sm: &dyn ShaderModel, f: &mut OpFoldData<'_>);
}

pub trait DisplayOp: DstsAsSlice {
    fn fmt_dsts(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt_dst_slice(f, self.dsts_as_slice())
    }

    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result;
}

// Hack struct so we can re-use Formatters.  Shamelessly stolen from
// https://users.rust-lang.org/t/reusing-an-fmt-formatter/8531/4
pub struct Fmt<F>(pub F)
where
    F: Fn(&mut fmt::Formatter) -> fmt::Result;

impl<F> fmt::Display for Fmt<F>
where
    F: Fn(&mut fmt::Formatter) -> fmt::Result,
{
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        (self.0)(f)
    }
}

macro_rules! impl_display_for_op {
    ($op: ident) => {
        impl fmt::Display for $op {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                let mut s = String::new();
                write!(s, "{}", Fmt(|f| self.fmt_dsts(f)))?;
                if !s.is_empty() {
                    write!(f, "{} = ", s)?;
                }
                self.fmt_op(f)
            }
        }
    };
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum PredSetOp {
    And,
    Or,
    Xor,
}

impl PredSetOp {
    pub fn eval(&self, a: bool, b: bool) -> bool {
        match self {
            PredSetOp::And => a & b,
            PredSetOp::Or => a | b,
            PredSetOp::Xor => a ^ b,
        }
    }

    pub fn is_trivial(&self, accum: &Src) -> bool {
        if let Some(b) = accum.as_bool() {
            match self {
                PredSetOp::And => b,
                PredSetOp::Or => !b,
                PredSetOp::Xor => !b,
            }
        } else {
            false
        }
    }
}

impl fmt::Display for PredSetOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PredSetOp::And => write!(f, ".and"),
            PredSetOp::Or => write!(f, ".or"),
            PredSetOp::Xor => write!(f, ".xor"),
        }
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum FloatCmpOp {
    OrdEq,
    OrdNe,
    OrdLt,
    OrdLe,
    OrdGt,
    OrdGe,
    UnordEq,
    UnordNe,
    UnordLt,
    UnordLe,
    UnordGt,
    UnordGe,
    IsNum,
    IsNan,
}

impl FloatCmpOp {
    pub fn flip(self) -> FloatCmpOp {
        match self {
            FloatCmpOp::OrdEq | FloatCmpOp::OrdNe => self,
            FloatCmpOp::OrdLt => FloatCmpOp::OrdGt,
            FloatCmpOp::OrdLe => FloatCmpOp::OrdGe,
            FloatCmpOp::OrdGt => FloatCmpOp::OrdLt,
            FloatCmpOp::OrdGe => FloatCmpOp::OrdLe,
            FloatCmpOp::UnordEq | FloatCmpOp::UnordNe => self,
            FloatCmpOp::UnordLt => FloatCmpOp::UnordGt,
            FloatCmpOp::UnordLe => FloatCmpOp::UnordGe,
            FloatCmpOp::UnordGt => FloatCmpOp::UnordLt,
            FloatCmpOp::UnordGe => FloatCmpOp::UnordLe,
            FloatCmpOp::IsNum | FloatCmpOp::IsNan => panic!("Cannot flip unop"),
        }
    }
}

impl fmt::Display for FloatCmpOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            FloatCmpOp::OrdEq => write!(f, ".eq"),
            FloatCmpOp::OrdNe => write!(f, ".ne"),
            FloatCmpOp::OrdLt => write!(f, ".lt"),
            FloatCmpOp::OrdLe => write!(f, ".le"),
            FloatCmpOp::OrdGt => write!(f, ".gt"),
            FloatCmpOp::OrdGe => write!(f, ".ge"),
            FloatCmpOp::UnordEq => write!(f, ".equ"),
            FloatCmpOp::UnordNe => write!(f, ".neu"),
            FloatCmpOp::UnordLt => write!(f, ".ltu"),
            FloatCmpOp::UnordLe => write!(f, ".leu"),
            FloatCmpOp::UnordGt => write!(f, ".gtu"),
            FloatCmpOp::UnordGe => write!(f, ".geu"),
            FloatCmpOp::IsNum => write!(f, ".num"),
            FloatCmpOp::IsNan => write!(f, ".nan"),
        }
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum IntCmpOp {
    False,
    True,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
}

impl IntCmpOp {
    pub fn flip(self) -> IntCmpOp {
        match self {
            IntCmpOp::False | IntCmpOp::True => self,
            IntCmpOp::Eq | IntCmpOp::Ne => self,
            IntCmpOp::Lt => IntCmpOp::Gt,
            IntCmpOp::Le => IntCmpOp::Ge,
            IntCmpOp::Gt => IntCmpOp::Lt,
            IntCmpOp::Ge => IntCmpOp::Le,
        }
    }
}

impl fmt::Display for IntCmpOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            IntCmpOp::False => write!(f, ".f"),
            IntCmpOp::True => write!(f, ".t"),
            IntCmpOp::Eq => write!(f, ".eq"),
            IntCmpOp::Ne => write!(f, ".ne"),
            IntCmpOp::Lt => write!(f, ".lt"),
            IntCmpOp::Le => write!(f, ".le"),
            IntCmpOp::Gt => write!(f, ".gt"),
            IntCmpOp::Ge => write!(f, ".ge"),
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum IntCmpType {
    U32,
    I32,
}

impl IntCmpType {
    #[allow(dead_code)]
    pub fn is_signed(&self) -> bool {
        match self {
            IntCmpType::U32 => false,
            IntCmpType::I32 => true,
        }
    }
}

impl fmt::Display for IntCmpType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            IntCmpType::U32 => write!(f, ".u32"),
            IntCmpType::I32 => write!(f, ".i32"),
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum LogicOp2 {
    And,
    Or,
    Xor,
    PassB,
}

impl fmt::Display for LogicOp2 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LogicOp2::And => write!(f, "and"),
            LogicOp2::Or => write!(f, "or"),
            LogicOp2::Xor => write!(f, "xor"),
            LogicOp2::PassB => write!(f, "pass_b"),
        }
    }
}

impl LogicOp2 {
    pub fn to_lut(self) -> LogicOp3 {
        match self {
            LogicOp2::And => LogicOp3::new_lut(&|x, y, _| x & y),
            LogicOp2::Or => LogicOp3::new_lut(&|x, y, _| x | y),
            LogicOp2::Xor => LogicOp3::new_lut(&|x, y, _| x ^ y),
            LogicOp2::PassB => LogicOp3::new_lut(&|_, b, _| b),
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub struct LogicOp3 {
    pub lut: u8,
}

impl LogicOp3 {
    pub const SRC_MASKS: [u8; 3] = [0xf0, 0xcc, 0xaa];

    #[inline]
    pub fn new_lut<F: Fn(u8, u8, u8) -> u8>(f: &F) -> LogicOp3 {
        LogicOp3 {
            lut: f(
                LogicOp3::SRC_MASKS[0],
                LogicOp3::SRC_MASKS[1],
                LogicOp3::SRC_MASKS[2],
            ),
        }
    }

    pub fn new_const(val: bool) -> LogicOp3 {
        LogicOp3 {
            lut: if val { !0 } else { 0 },
        }
    }

    pub fn src_used(&self, src_idx: usize) -> bool {
        let mask = LogicOp3::SRC_MASKS[src_idx];
        let shift = LogicOp3::SRC_MASKS[src_idx].trailing_zeros();
        self.lut & !mask != (self.lut >> shift) & !mask
    }

    pub fn fix_src(&mut self, src_idx: usize, val: bool) {
        let mask = LogicOp3::SRC_MASKS[src_idx];
        let shift = LogicOp3::SRC_MASKS[src_idx].trailing_zeros();
        if val {
            let t_bits = self.lut & mask;
            self.lut = t_bits | (t_bits >> shift)
        } else {
            let f_bits = self.lut & !mask;
            self.lut = (f_bits << shift) | f_bits
        };
    }

    pub fn invert_src(&mut self, src_idx: usize) {
        let mask = LogicOp3::SRC_MASKS[src_idx];
        let shift = LogicOp3::SRC_MASKS[src_idx].trailing_zeros();
        let t_bits = self.lut & mask;
        let f_bits = self.lut & !mask;
        self.lut = (f_bits << shift) | (t_bits >> shift);
    }

    pub fn eval<
        T: BitAnd<Output = T> + BitOr<Output = T> + Copy + Not<Output = T>,
    >(
        &self,
        x: T,
        y: T,
        z: T,
    ) -> T {
        let mut res = x & !x; // zero
        if (self.lut & (1 << 0)) != 0 {
            res = res | (!x & !y & !z);
        }
        if (self.lut & (1 << 1)) != 0 {
            res = res | (!x & !y & z);
        }
        if (self.lut & (1 << 2)) != 0 {
            res = res | (!x & y & !z);
        }
        if (self.lut & (1 << 3)) != 0 {
            res = res | (!x & y & z);
        }
        if (self.lut & (1 << 4)) != 0 {
            res = res | (x & !y & !z);
        }
        if (self.lut & (1 << 5)) != 0 {
            res = res | (x & !y & z);
        }
        if (self.lut & (1 << 6)) != 0 {
            res = res | (x & y & !z);
        }
        if (self.lut & (1 << 7)) != 0 {
            res = res | (x & y & z);
        }
        res
    }
}

impl fmt::Display for LogicOp3 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "LUT[{:#x}]", self.lut)
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum FloatType {
    F16,
    F32,
    F64,
}

impl FloatType {
    pub fn from_bits(bytes: usize) -> FloatType {
        match bytes {
            16 => FloatType::F16,
            32 => FloatType::F32,
            64 => FloatType::F64,
            _ => panic!("Invalid float type size"),
        }
    }

    pub fn bits(&self) -> usize {
        match self {
            FloatType::F16 => 16,
            FloatType::F32 => 32,
            FloatType::F64 => 64,
        }
    }
}

impl fmt::Display for FloatType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            FloatType::F16 => write!(f, ".f16"),
            FloatType::F32 => write!(f, ".f32"),
            FloatType::F64 => write!(f, ".f64"),
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum FRndMode {
    NearestEven,
    NegInf,
    PosInf,
    Zero,
}

impl fmt::Display for FRndMode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            FRndMode::NearestEven => write!(f, ".re"),
            FRndMode::NegInf => write!(f, ".rm"),
            FRndMode::PosInf => write!(f, ".rp"),
            FRndMode::Zero => write!(f, ".rz"),
        }
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub struct TexCBufRef {
    pub idx: u8,
    pub offset: u16,
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum TexRef {
    Bound(u16),
    CBuf(TexCBufRef),
    Bindless,
}

impl fmt::Display for TexRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            TexRef::Bound(idx) => write!(f, "tex[{idx}]"),
            TexRef::CBuf(TexCBufRef { idx, offset }) => {
                write!(f, "c[{idx:#x}][{offset:#x}]")
            }
            TexRef::Bindless => write!(f, "bindless"),
        }
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum TexDim {
    _1D,
    Array1D,
    _2D,
    Array2D,
    _3D,
    Cube,
    ArrayCube,
}

impl fmt::Display for TexDim {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            TexDim::_1D => write!(f, ".1d"),
            TexDim::Array1D => write!(f, ".a1d"),
            TexDim::_2D => write!(f, ".2d"),
            TexDim::Array2D => write!(f, ".a2d"),
            TexDim::_3D => write!(f, ".3d"),
            TexDim::Cube => write!(f, ".cube"),
            TexDim::ArrayCube => write!(f, ".acube"),
        }
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum TexLodMode {
    Auto,
    Zero,
    Bias,
    Lod,
    Clamp,
    BiasClamp,
}

impl TexLodMode {
    pub fn is_explicit_lod(&self) -> bool {
        match self {
            TexLodMode::Auto
            | TexLodMode::Bias
            | TexLodMode::Clamp
            | TexLodMode::BiasClamp => false,
            TexLodMode::Zero | TexLodMode::Lod => true,
        }
    }
}

impl fmt::Display for TexLodMode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            TexLodMode::Auto => write!(f, ""),
            TexLodMode::Zero => write!(f, ".lz"),
            TexLodMode::Bias => write!(f, ".lb"),
            TexLodMode::Lod => write!(f, ".ll"),
            TexLodMode::Clamp => write!(f, ".lc"),
            TexLodMode::BiasClamp => write!(f, ".lb.lc"),
        }
    }
}

/// Derivative behavior for tex ops and FSwzAdd
///
/// The descriptions here may not be wholly accurate as they come from cobbling
/// together a bunch of pieces.  This is my (Faith's) best understanding of how
/// these things work.
#[allow(dead_code)]
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum TexDerivMode {
    /// Automatic
    ///
    /// For partial (not full) quads, the derivative will default to the value
    /// of DEFAULT_PARTIAL in SET_SHADER_CONTROL.
    ///
    /// On Volta and earlier GPUs or on Blackwell B and later, derivatives in
    /// all non-fragment shaders stages are assumed to be partial.
    Auto,

    /// Assume a non-divergent (full) derivative
    ///
    /// Partial derivative checks are skipped and the hardware does the
    /// derivative anyway, possibly on rubbish data.
    NonDivergent,

    /// Force the derivative to be considered divergent (partial)
    ///
    /// This only exists as a separate thing on Blackwell A.  On Hopper and
    /// earlier, there is a .fdv that's part of the LodMode, but only for
    /// LodMode::Clamp.  On Blackwell B, it appears (according to the
    /// disassembler) to be removed again in favor of DerivXY.
    ForceDivergent,

    /// Attempt an X/Y derivative, ignoring shader stage
    ///
    /// This is (I think) identical to Auto except that it ignores the shader
    /// stage checks.  This is new on Blackwell B+.
    DerivXY,
}

impl fmt::Display for TexDerivMode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            TexDerivMode::Auto => Ok(()),
            TexDerivMode::NonDivergent => write!(f, ".ndv"),
            TexDerivMode::ForceDivergent => write!(f, ".fdv"),
            TexDerivMode::DerivXY => write!(f, ".dxy"),
        }
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub struct ChannelMask(u8);

impl ChannelMask {
    pub fn new(mask: u8) -> Self {
        assert!(mask != 0 && (mask & !0xf) == 0);
        ChannelMask(mask)
    }

    pub fn for_comps(comps: u8) -> Self {
        assert!(comps > 0 && comps <= 4);
        ChannelMask((1 << comps) - 1)
    }

    pub fn to_bits(self) -> u8 {
        self.0
    }
}

impl fmt::Display for ChannelMask {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, ".")?;
        for (i, c) in ['r', 'g', 'b', 'a'].into_iter().enumerate() {
            if self.0 & (1 << i) != 0 {
                write!(f, "{c}")?;
            }
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum TexOffsetMode {
    None,
    AddOffI,
    PerPx, // tld4 only
}

impl fmt::Display for TexOffsetMode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            TexOffsetMode::None => write!(f, ""),
            TexOffsetMode::AddOffI => write!(f, ".aoffi"),
            TexOffsetMode::PerPx => write!(f, ".ptp"),
        }
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum TexQuery {
    Dimension,
    TextureType,
    SamplerPos,
}

impl fmt::Display for TexQuery {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            TexQuery::Dimension => write!(f, "dimension"),
            TexQuery::TextureType => write!(f, "texture_type"),
            TexQuery::SamplerPos => write!(f, "sampler_pos"),
        }
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum ImageDim {
    _1D,
    _1DBuffer,
    _1DArray,
    _2D,
    _2DArray,
    _3D,
}

impl ImageDim {
    pub fn coord_comps(&self) -> u8 {
        match self {
            ImageDim::_1D => 1,
            ImageDim::_1DBuffer => 1,
            ImageDim::_1DArray => 2,
            ImageDim::_2D => 2,
            ImageDim::_2DArray => 3,
            ImageDim::_3D => 3,
        }
    }
}

impl fmt::Display for ImageDim {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ImageDim::_1D => write!(f, ".1d"),
            ImageDim::_1DBuffer => write!(f, ".buf"),
            ImageDim::_1DArray => write!(f, ".a1d"),
            ImageDim::_2D => write!(f, ".2d"),
            ImageDim::_2DArray => write!(f, ".a2d"),
            ImageDim::_3D => write!(f, ".3d"),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum IntType {
    U8,
    I8,
    U16,
    I16,
    U32,
    I32,
    U64,
    I64,
}

impl IntType {
    pub fn from_bits(bits: usize, is_signed: bool) -> IntType {
        match bits {
            8 => {
                if is_signed {
                    IntType::I8
                } else {
                    IntType::U8
                }
            }
            16 => {
                if is_signed {
                    IntType::I16
                } else {
                    IntType::U16
                }
            }
            32 => {
                if is_signed {
                    IntType::I32
                } else {
                    IntType::U32
                }
            }
            64 => {
                if is_signed {
                    IntType::I64
                } else {
                    IntType::U64
                }
            }
            _ => panic!("Invalid integer type size"),
        }
    }

    pub fn is_signed(&self) -> bool {
        match self {
            IntType::U8 | IntType::U16 | IntType::U32 | IntType::U64 => false,
            IntType::I8 | IntType::I16 | IntType::I32 | IntType::I64 => true,
        }
    }

    pub fn bits(&self) -> usize {
        match self {
            IntType::U8 | IntType::I8 => 8,
            IntType::U16 | IntType::I16 => 16,
            IntType::U32 | IntType::I32 => 32,
            IntType::U64 | IntType::I64 => 64,
        }
    }
}

impl fmt::Display for IntType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            IntType::U8 => write!(f, ".u8"),
            IntType::I8 => write!(f, ".i8"),
            IntType::U16 => write!(f, ".u16"),
            IntType::I16 => write!(f, ".i16"),
            IntType::U32 => write!(f, ".u32"),
            IntType::I32 => write!(f, ".i32"),
            IntType::U64 => write!(f, ".u64"),
            IntType::I64 => write!(f, ".i64"),
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum MemAddrType {
    A32,
    A64,
}

impl fmt::Display for MemAddrType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MemAddrType::A32 => write!(f, ".a32"),
            MemAddrType::A64 => write!(f, ".a64"),
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum MemType {
    U8,
    I8,
    U16,
    I16,
    B32,
    B64,
    B128,
}

impl MemType {
    pub fn from_size(size: u8, is_signed: bool) -> MemType {
        match size {
            1 => {
                if is_signed {
                    MemType::I8
                } else {
                    MemType::U8
                }
            }
            2 => {
                if is_signed {
                    MemType::I16
                } else {
                    MemType::U16
                }
            }
            4 => MemType::B32,
            8 => MemType::B64,
            16 => MemType::B128,
            _ => panic!("Invalid memory load/store size"),
        }
    }

    #[allow(dead_code)]
    pub fn bits(&self) -> usize {
        match self {
            MemType::U8 | MemType::I8 => 8,
            MemType::U16 | MemType::I16 => 16,
            MemType::B32 => 32,
            MemType::B64 => 64,
            MemType::B128 => 128,
        }
    }
}

impl fmt::Display for MemType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MemType::U8 => write!(f, ".u8"),
            MemType::I8 => write!(f, ".i8"),
            MemType::U16 => write!(f, ".u16"),
            MemType::I16 => write!(f, ".i16"),
            MemType::B32 => write!(f, ".b32"),
            MemType::B64 => write!(f, ".b64"),
            MemType::B128 => write!(f, ".b128"),
        }
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum MemOrder {
    Constant,
    Weak,
    Strong(MemScope),
}

impl fmt::Display for MemOrder {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MemOrder::Constant => write!(f, ".constant"),
            MemOrder::Weak => write!(f, ".weak"),
            MemOrder::Strong(scope) => write!(f, ".strong{}", scope),
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum MemScope {
    CTA,
    GPU,
    System,
}

impl fmt::Display for MemScope {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MemScope::CTA => write!(f, ".cta"),
            MemScope::GPU => write!(f, ".gpu"),
            MemScope::System => write!(f, ".sys"),
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum MemSpace {
    Global(MemAddrType),
    Local,
    Shared,
}

impl MemSpace {
    pub fn addr_type(&self) -> MemAddrType {
        match self {
            MemSpace::Global(t) => *t,
            MemSpace::Local => MemAddrType::A32,
            MemSpace::Shared => MemAddrType::A32,
        }
    }
}

impl fmt::Display for MemSpace {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MemSpace::Global(t) => write!(f, ".global{t}"),
            MemSpace::Local => write!(f, ".local"),
            MemSpace::Shared => write!(f, ".shared"),
        }
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum MemEvictionPriority {
    First,
    Normal,
    Last,
    LastUse,
    Unchanged,
    NoAllocate,
}

impl fmt::Display for MemEvictionPriority {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MemEvictionPriority::First => write!(f, ".ef"),
            MemEvictionPriority::Normal => Ok(()),
            MemEvictionPriority::Last => write!(f, ".el"),
            MemEvictionPriority::LastUse => write!(f, ".lu"),
            MemEvictionPriority::Unchanged => write!(f, ".eu"),
            MemEvictionPriority::NoAllocate => write!(f, ".na"),
        }
    }
}

/// Memory load cache ops used by Kepler
#[allow(dead_code)]
#[derive(Clone, Copy, Default, Eq, Hash, PartialEq)]
pub enum LdCacheOp {
    #[default]
    CacheAll,
    CacheGlobal,
    /// This cache mode not officially documented by NVIDIA.  What we do know is
    /// that the Cuda C programming gude says:
    ///
    /// > The read-only data cache load function is only supported by devices
    /// > of compute capability 5.0 and higher.
    /// > ```c
    /// > T __ldg(const T* address);
    /// > ```
    ///
    /// and we know that `__ldg()` compiles to `ld.global.nc` in PTX which
    /// compiles to `ld.ci`.  The PTX 5.0 docs say:
    ///
    /// > Load register variable `d` from the location specified by the source
    /// > address operand `a` in the global state space, and optionally cache in
    /// > non-coherent texture cache. Since the cache is non-coherent, the data
    /// > should be read-only within the kernel's process.
    ///
    /// Since `.nc` means "non-coherent", the name "incoherent" seems about
    /// right.  The quote above also seems to imply that these loads got loaded
    /// through the texture cache but we don't fully understand the implications
    /// of that.
    CacheIncoherent,
    CacheStreaming,
    CacheInvalidate,
}

impl fmt::Display for LdCacheOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LdCacheOp::CacheAll => write!(f, ".ca"),
            LdCacheOp::CacheGlobal => write!(f, ".cg"),
            LdCacheOp::CacheIncoherent => write!(f, ".ci"),
            LdCacheOp::CacheStreaming => write!(f, ".cs"),
            LdCacheOp::CacheInvalidate => write!(f, ".cv"),
        }
    }
}

impl LdCacheOp {
    pub fn select(
        sm: &dyn ShaderModel,
        space: MemSpace,
        order: MemOrder,
        _eviction_priority: MemEvictionPriority,
    ) -> Self {
        match space {
            MemSpace::Global(_) => match order {
                MemOrder::Constant => {
                    if sm.sm() >= 50 {
                        // This is undocumented in the CUDA docs but NVIDIA uses
                        // it for constant loads.
                        LdCacheOp::CacheIncoherent
                    } else {
                        LdCacheOp::CacheAll
                    }
                }
                MemOrder::Strong(MemScope::System) => {
                    LdCacheOp::CacheInvalidate
                }
                _ => {
                    // From the CUDA 10.2 docs:
                    //
                    //    "The default load instruction cache operation is
                    //    ld.ca, which allocates cache lines in all levels (L1
                    //    and L2) with normal eviction policy. Global data is
                    //    coherent at the L2 level, but multiple L1 caches are
                    //    not coherent for global data. If one thread stores to
                    //    global memory via one L1 cache, and a second thread
                    //    loads that address via a second L1 cache with ld.ca,
                    //    the second thread may get stale L1 cache data"
                    //
                    // and
                    //
                    //    "L1 caching in Kepler GPUs is reserved only for local
                    //    memory accesses, such as register spills and stack
                    //    data. Global loads are cached in L2 only (or in the
                    //    Read-Only Data Cache)."
                    //
                    // We follow suit and use CacheGlobal for all global memory
                    // access on Kepler.  On Maxwell, it appears safe to use
                    // CacheAll for everything.
                    if sm.sm() >= 50 {
                        LdCacheOp::CacheAll
                    } else {
                        LdCacheOp::CacheGlobal
                    }
                }
            },
            MemSpace::Local | MemSpace::Shared => LdCacheOp::CacheAll,
        }
    }
}

/// Memory store cache ops used by Kepler
#[allow(dead_code)]
#[derive(Clone, Copy, Default, Eq, Hash, PartialEq)]
pub enum StCacheOp {
    #[default]
    WriteBack,
    CacheGlobal,
    CacheStreaming,
    WriteThrough,
}

impl fmt::Display for StCacheOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            StCacheOp::WriteBack => write!(f, ".wb"),
            StCacheOp::CacheGlobal => write!(f, ".cg"),
            StCacheOp::CacheStreaming => write!(f, ".cs"),
            StCacheOp::WriteThrough => write!(f, ".wt"),
        }
    }
}

impl StCacheOp {
    pub fn select(
        sm: &dyn ShaderModel,
        space: MemSpace,
        order: MemOrder,
        _eviction_priority: MemEvictionPriority,
    ) -> Self {
        match space {
            MemSpace::Global(_) => match order {
                MemOrder::Constant => panic!("Cannot store to constant"),
                MemOrder::Strong(MemScope::System) => StCacheOp::WriteThrough,
                _ => {
                    // See the corresponding comment in LdCacheOp::select()
                    if sm.sm() >= 50 {
                        StCacheOp::WriteBack
                    } else {
                        StCacheOp::CacheGlobal
                    }
                }
            },
            MemSpace::Local | MemSpace::Shared => StCacheOp::WriteBack,
        }
    }
}

#[derive(Clone)]
pub struct MemAccess {
    pub mem_type: MemType,
    pub space: MemSpace,
    pub order: MemOrder,
    pub eviction_priority: MemEvictionPriority,
}

impl fmt::Display for MemAccess {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}{}{}{}",
            self.space, self.order, self.eviction_priority, self.mem_type,
        )
    }
}

impl MemAccess {
    pub fn ld_cache_op(&self, sm: &dyn ShaderModel) -> LdCacheOp {
        LdCacheOp::select(sm, self.space, self.order, self.eviction_priority)
    }

    pub fn st_cache_op(&self, sm: &dyn ShaderModel) -> StCacheOp {
        StCacheOp::select(sm, self.space, self.order, self.eviction_priority)
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum AtomType {
    F16x2,
    U32,
    I32,
    F32,
    U64,
    I64,
    F64,
}

impl AtomType {
    pub fn F(bits: u8) -> AtomType {
        match bits {
            16 => panic!("16-bit float atomics not yet supported"),
            32 => AtomType::F32,
            64 => AtomType::F64,
            _ => panic!("Invalid float atomic type"),
        }
    }

    pub fn U(bits: u8) -> AtomType {
        match bits {
            32 => AtomType::U32,
            64 => AtomType::U64,
            _ => panic!("Invalid uint atomic type"),
        }
    }

    pub fn I(bits: u8) -> AtomType {
        match bits {
            32 => AtomType::I32,
            64 => AtomType::I64,
            _ => panic!("Invalid int atomic type"),
        }
    }

    pub fn bits(&self) -> usize {
        match self {
            AtomType::F16x2 | AtomType::F32 => 32,
            AtomType::U32 | AtomType::I32 => 32,
            AtomType::U64 | AtomType::I64 | AtomType::F64 => 64,
        }
    }

    pub fn is_float(&self) -> bool {
        match self {
            AtomType::F16x2 | AtomType::F32 | AtomType::F64 => true,
            AtomType::U32 | AtomType::I32 | AtomType::U64 | AtomType::I64 => {
                false
            }
        }
    }
}

impl fmt::Display for AtomType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AtomType::F16x2 => write!(f, ".f16x2"),
            AtomType::U32 => write!(f, ".u32"),
            AtomType::I32 => write!(f, ".i32"),
            AtomType::F32 => write!(f, ".f32"),
            AtomType::U64 => write!(f, ".u64"),
            AtomType::I64 => write!(f, ".i64"),
            AtomType::F64 => write!(f, ".f64"),
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum AtomCmpSrc {
    /// The cmpr value is passed as a separate source
    Separate,
    /// The cmpr value is packed in with the data with cmpr coming first
    Packed,
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum AtomOp {
    Add,
    Min,
    Max,
    Inc,
    Dec,
    And,
    Or,
    Xor,
    Exch,
    CmpExch(AtomCmpSrc),
}

impl AtomOp {
    pub fn is_reduction(&self) -> bool {
        match self {
            AtomOp::Add
            | AtomOp::Min
            | AtomOp::Max
            | AtomOp::Inc
            | AtomOp::Dec
            | AtomOp::And
            | AtomOp::Or
            | AtomOp::Xor => true,
            AtomOp::Exch | AtomOp::CmpExch(_) => false,
        }
    }
}

impl fmt::Display for AtomOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AtomOp::Add => write!(f, ".add"),
            AtomOp::Min => write!(f, ".min"),
            AtomOp::Max => write!(f, ".max"),
            AtomOp::Inc => write!(f, ".inc"),
            AtomOp::Dec => write!(f, ".dec"),
            AtomOp::And => write!(f, ".and"),
            AtomOp::Or => write!(f, ".or"),
            AtomOp::Xor => write!(f, ".xor"),
            AtomOp::Exch => write!(f, ".exch"),
            AtomOp::CmpExch(AtomCmpSrc::Separate) => write!(f, ".cmpexch"),
            AtomOp::CmpExch(AtomCmpSrc::Packed) => write!(f, ".cmpexch.packed"),
        }
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum InterpFreq {
    Pass,
    PassMulW,
    Constant,
    State,
}

impl fmt::Display for InterpFreq {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            InterpFreq::Pass => write!(f, ".pass"),
            InterpFreq::PassMulW => write!(f, ".pass_mul_w"),
            InterpFreq::Constant => write!(f, ".constant"),
            InterpFreq::State => write!(f, ".state"),
        }
    }
}
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum InterpLoc {
    Default,
    Centroid,
    Offset,
}

impl fmt::Display for InterpLoc {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            InterpLoc::Default => Ok(()),
            InterpLoc::Centroid => write!(f, ".centroid"),
            InterpLoc::Offset => write!(f, ".offset"),
        }
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpFAdd {
    #[dst_type(F32)]
    pub dst: Dst,

    #[src_type(F32)]
    pub srcs: [Src; 2],

    pub saturate: bool,
    pub rnd_mode: FRndMode,
    pub ftz: bool,
}

impl DisplayOp for OpFAdd {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let sat = if self.saturate { ".sat" } else { "" };
        write!(f, "fadd{sat}")?;
        if self.rnd_mode != FRndMode::NearestEven {
            write!(f, "{}", self.rnd_mode)?;
        }
        if self.ftz {
            write!(f, ".ftz")?;
        }
        write!(f, " {} {}", self.srcs[0], self.srcs[1],)
    }
}
impl_display_for_op!(OpFAdd);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpFFma {
    #[dst_type(F32)]
    pub dst: Dst,

    #[src_type(F32)]
    pub srcs: [Src; 3],

    pub saturate: bool,
    pub rnd_mode: FRndMode,
    pub ftz: bool,
    pub dnz: bool,
}

impl DisplayOp for OpFFma {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let sat = if self.saturate { ".sat" } else { "" };
        write!(f, "ffma{sat}")?;
        if self.rnd_mode != FRndMode::NearestEven {
            write!(f, "{}", self.rnd_mode)?;
        }
        if self.dnz {
            write!(f, ".dnz")?;
        } else if self.ftz {
            write!(f, ".ftz")?;
        }
        write!(f, " {} {} {}", self.srcs[0], self.srcs[1], self.srcs[2])
    }
}
impl_display_for_op!(OpFFma);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpFMnMx {
    #[dst_type(F32)]
    pub dst: Dst,

    #[src_type(F32)]
    pub srcs: [Src; 2],

    #[src_type(Pred)]
    pub min: Src,

    pub ftz: bool,
}

impl DisplayOp for OpFMnMx {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let ftz = if self.ftz { ".ftz" } else { "" };
        write!(
            f,
            "fmnmx{ftz} {} {} {}",
            self.srcs[0], self.srcs[1], self.min
        )
    }
}
impl_display_for_op!(OpFMnMx);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpFMul {
    #[dst_type(F32)]
    pub dst: Dst,

    #[src_type(F32)]
    pub srcs: [Src; 2],

    pub saturate: bool,
    pub rnd_mode: FRndMode,
    pub ftz: bool,
    pub dnz: bool,
}

impl DisplayOp for OpFMul {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let sat = if self.saturate { ".sat" } else { "" };
        write!(f, "fmul{sat}")?;
        if self.rnd_mode != FRndMode::NearestEven {
            write!(f, "{}", self.rnd_mode)?;
        }
        if self.dnz {
            write!(f, ".dnz")?;
        } else if self.ftz {
            write!(f, ".ftz")?;
        }
        write!(f, " {} {}", self.srcs[0], self.srcs[1],)
    }
}
impl_display_for_op!(OpFMul);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpFSet {
    #[dst_type(F32)]
    pub dst: Dst,

    pub cmp_op: FloatCmpOp,

    #[src_type(F32)]
    pub srcs: [Src; 2],

    pub ftz: bool,
}

impl DisplayOp for OpFSet {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let ftz = if self.ftz { ".ftz" } else { "" };
        write!(
            f,
            "fset{}{ftz} {} {}",
            self.cmp_op, self.srcs[0], self.srcs[1]
        )
    }
}
impl_display_for_op!(OpFSet);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpFSetP {
    #[dst_type(Pred)]
    pub dst: Dst,

    pub set_op: PredSetOp,
    pub cmp_op: FloatCmpOp,

    #[src_type(F32)]
    pub srcs: [Src; 2],

    #[src_type(Pred)]
    pub accum: Src,

    pub ftz: bool,
}

impl DisplayOp for OpFSetP {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let ftz = if self.ftz { ".ftz" } else { "" };
        write!(f, "fsetp{}{ftz}", self.cmp_op)?;
        if !self.set_op.is_trivial(&self.accum) {
            write!(f, "{}", self.set_op)?;
        }
        write!(f, " {} {}", self.srcs[0], self.srcs[1])?;
        if !self.set_op.is_trivial(&self.accum) {
            write!(f, " {}", self.accum)?;
        }
        Ok(())
    }
}
impl_display_for_op!(OpFSetP);

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum FSwzAddOp {
    Add,
    SubRight,
    SubLeft,
    MoveLeft,
}

impl fmt::Display for FSwzAddOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            FSwzAddOp::Add => write!(f, "add"),
            FSwzAddOp::SubRight => write!(f, "subr"),
            FSwzAddOp::SubLeft => write!(f, "sub"),
            FSwzAddOp::MoveLeft => write!(f, "mov2"),
        }
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpFSwzAdd {
    #[dst_type(F32)]
    pub dst: Dst,

    #[src_type(GPR)]
    pub srcs: [Src; 2],

    pub rnd_mode: FRndMode,
    pub ftz: bool,
    pub deriv_mode: TexDerivMode,

    pub ops: [FSwzAddOp; 4],
}

impl DisplayOp for OpFSwzAdd {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "fswzadd",)?;
        if self.rnd_mode != FRndMode::NearestEven {
            write!(f, "{}", self.rnd_mode)?;
        }
        if self.ftz {
            write!(f, ".ftz")?;
        }
        write!(f, "{}", self.deriv_mode)?;
        write!(
            f,
            " {} {} [{}, {}, {}, {}]",
            self.srcs[0],
            self.srcs[1],
            self.ops[0],
            self.ops[1],
            self.ops[2],
            self.ops[3],
        )
    }
}
impl_display_for_op!(OpFSwzAdd);

/// Describes where the second src is taken before doing the ops
#[allow(dead_code)]
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum FSwzShuffle {
    Quad0,
    Quad1,
    Quad2,
    Quad3,
    // swap [0, 1] and [2, 3]
    SwapHorizontal,
    // swap [0, 2] and [1, 3]
    SwapVertical,
}

impl fmt::Display for FSwzShuffle {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            FSwzShuffle::Quad0 => write!(f, ".0000"),
            FSwzShuffle::Quad1 => write!(f, ".1111"),
            FSwzShuffle::Quad2 => write!(f, ".2222"),
            FSwzShuffle::Quad3 => write!(f, ".3333"),
            FSwzShuffle::SwapHorizontal => write!(f, ".1032"),
            FSwzShuffle::SwapVertical => write!(f, ".2301"),
        }
    }
}

/// Op only present in Kepler and older
/// It first does a shuffle on the second src and then applies
/// src0 op src1, each thread on a quad might do a different operation.
///
/// This is used to encode ddx/ddy
/// ex: ddx
///   src1 = shuffle swap horizontal src1
///   ops = [sub, subr, sub, subr]
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpFSwz {
    #[dst_type(F32)]
    pub dst: Dst,

    #[src_type(GPR)]
    pub srcs: [Src; 2],

    pub rnd_mode: FRndMode,
    pub ftz: bool,
    pub deriv_mode: TexDerivMode,
    pub shuffle: FSwzShuffle,

    pub ops: [FSwzAddOp; 4],
}

impl DisplayOp for OpFSwz {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "fswz{}", self.shuffle)?;
        if self.rnd_mode != FRndMode::NearestEven {
            write!(f, "{}", self.rnd_mode)?;
        }
        write!(f, "{}", self.deriv_mode)?;
        if self.ftz {
            write!(f, ".ftz")?;
        }
        write!(
            f,
            " {} {} [{}, {}, {}, {}]",
            self.srcs[0],
            self.srcs[1],
            self.ops[0],
            self.ops[1],
            self.ops[2],
            self.ops[3],
        )
    }
}
impl_display_for_op!(OpFSwz);

pub enum RroOp {
    SinCos,
    Exp2,
}

impl fmt::Display for RroOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            RroOp::SinCos => write!(f, ".sincos"),
            RroOp::Exp2 => write!(f, ".exp2"),
        }
    }
}

/// MuFu range reduction operator
///
/// Not available on SM70+
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpRro {
    #[dst_type(F32)]
    pub dst: Dst,

    pub op: RroOp,

    #[src_type(F32)]
    pub src: Src,
}

impl DisplayOp for OpRro {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "rro{} {}", self.op, self.src)
    }
}
impl_display_for_op!(OpRro);

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum MuFuOp {
    Cos,
    Sin,
    Exp2,
    Log2,
    Rcp,
    Rsq,
    Rcp64H,
    Rsq64H,
    Sqrt,
    Tanh,
}

impl fmt::Display for MuFuOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MuFuOp::Cos => write!(f, "cos"),
            MuFuOp::Sin => write!(f, "sin"),
            MuFuOp::Exp2 => write!(f, "exp2"),
            MuFuOp::Log2 => write!(f, "log2"),
            MuFuOp::Rcp => write!(f, "rcp"),
            MuFuOp::Rsq => write!(f, "rsq"),
            MuFuOp::Rcp64H => write!(f, "rcp64h"),
            MuFuOp::Rsq64H => write!(f, "rsq64h"),
            MuFuOp::Sqrt => write!(f, "sqrt"),
            MuFuOp::Tanh => write!(f, "tanh"),
        }
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpMuFu {
    #[dst_type(F32)]
    pub dst: Dst,

    pub op: MuFuOp,

    #[src_type(F32)]
    pub src: Src,
}

impl DisplayOp for OpMuFu {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "mufu.{} {}", self.op, self.src)
    }
}
impl_display_for_op!(OpMuFu);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpDAdd {
    #[dst_type(F64)]
    pub dst: Dst,

    #[src_type(F64)]
    pub srcs: [Src; 2],

    pub rnd_mode: FRndMode,
}

impl DisplayOp for OpDAdd {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "dadd")?;
        if self.rnd_mode != FRndMode::NearestEven {
            write!(f, "{}", self.rnd_mode)?;
        }
        write!(f, " {} {}", self.srcs[0], self.srcs[1],)
    }
}
impl_display_for_op!(OpDAdd);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpDMul {
    #[dst_type(F64)]
    pub dst: Dst,

    #[src_type(F64)]
    pub srcs: [Src; 2],

    pub rnd_mode: FRndMode,
}

impl DisplayOp for OpDMul {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "dmul")?;
        if self.rnd_mode != FRndMode::NearestEven {
            write!(f, "{}", self.rnd_mode)?;
        }
        write!(f, " {} {}", self.srcs[0], self.srcs[1],)
    }
}
impl_display_for_op!(OpDMul);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpDFma {
    #[dst_type(F64)]
    pub dst: Dst,

    #[src_type(F64)]
    pub srcs: [Src; 3],

    pub rnd_mode: FRndMode,
}

impl DisplayOp for OpDFma {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "dfma")?;
        if self.rnd_mode != FRndMode::NearestEven {
            write!(f, "{}", self.rnd_mode)?;
        }
        write!(f, " {} {} {}", self.srcs[0], self.srcs[1], self.srcs[2])
    }
}
impl_display_for_op!(OpDFma);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpDMnMx {
    #[dst_type(F64)]
    pub dst: Dst,

    #[src_type(F64)]
    pub srcs: [Src; 2],

    #[src_type(Pred)]
    pub min: Src,
}

impl DisplayOp for OpDMnMx {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "dmnmx {} {} {}", self.srcs[0], self.srcs[1], self.min)
    }
}
impl_display_for_op!(OpDMnMx);

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpDSetP {
    #[dst_type(Pred)]
    pub dst: Dst,

    pub set_op: PredSetOp,
    pub cmp_op: FloatCmpOp,

    #[src_type(F64)]
    pub srcs: [Src; 2],

    #[src_type(Pred)]
    pub accum: Src,
}

impl Foldable for OpDSetP {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let a = f.get_f64_src(self, &self.srcs[0]);
        let b = f.get_f64_src(self, &self.srcs[1]);
        let accum = f.get_pred_src(self, &self.accum);

        let ordered = !a.is_nan() && !b.is_nan();
        let cmp_res = match self.cmp_op {
            FloatCmpOp::OrdEq => ordered && a == b,
            FloatCmpOp::OrdNe => ordered && a != b,
            FloatCmpOp::OrdLt => ordered && a < b,
            FloatCmpOp::OrdLe => ordered && a <= b,
            FloatCmpOp::OrdGt => ordered && a > b,
            FloatCmpOp::OrdGe => ordered && a >= b,
            FloatCmpOp::UnordEq => !ordered || a == b,
            FloatCmpOp::UnordNe => !ordered || a != b,
            FloatCmpOp::UnordLt => !ordered || a < b,
            FloatCmpOp::UnordLe => !ordered || a <= b,
            FloatCmpOp::UnordGt => !ordered || a > b,
            FloatCmpOp::UnordGe => !ordered || a >= b,
            FloatCmpOp::IsNum => ordered,
            FloatCmpOp::IsNan => !ordered,
        };
        let res = self.set_op.eval(cmp_res, accum);

        f.set_pred_dst(self, &self.dst, res);
    }
}

impl DisplayOp for OpDSetP {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "dsetp{}", self.cmp_op)?;
        if !self.set_op.is_trivial(&self.accum) {
            write!(f, "{}", self.set_op)?;
        }
        write!(f, " {} {}", self.srcs[0], self.srcs[1])?;
        if !self.set_op.is_trivial(&self.accum) {
            write!(f, " {}", self.accum)?;
        }
        Ok(())
    }
}
impl_display_for_op!(OpDSetP);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpHAdd2 {
    #[dst_type(F16v2)]
    pub dst: Dst,

    #[src_type(F16v2)]
    pub srcs: [Src; 2],

    pub saturate: bool,
    pub ftz: bool,
    pub f32: bool,
}

impl DisplayOp for OpHAdd2 {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let sat = if self.saturate { ".sat" } else { "" };
        let f32 = if self.f32 { ".f32" } else { "" };
        write!(f, "hadd2{sat}{f32}")?;
        if self.ftz {
            write!(f, ".ftz")?;
        }
        write!(f, " {} {}", self.srcs[0], self.srcs[1])
    }
}
impl_display_for_op!(OpHAdd2);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpHSet2 {
    #[dst_type(F16v2)]
    pub dst: Dst,

    pub set_op: PredSetOp,
    pub cmp_op: FloatCmpOp,

    #[src_type(F16v2)]
    pub srcs: [Src; 2],

    #[src_type(Pred)]
    pub accum: Src,

    pub ftz: bool,
}

impl DisplayOp for OpHSet2 {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let ftz = if self.ftz { ".ftz" } else { "" };
        write!(f, "hset2{}{ftz}", self.cmp_op)?;
        if !self.set_op.is_trivial(&self.accum) {
            write!(f, "{}", self.set_op)?;
        }
        write!(f, " {} {}", self.srcs[0], self.srcs[1])?;
        if !self.set_op.is_trivial(&self.accum) {
            write!(f, " {}", self.accum)?;
        }
        Ok(())
    }
}
impl_display_for_op!(OpHSet2);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpHSetP2 {
    #[dst_type(Pred)]
    pub dsts: [Dst; 2],

    pub set_op: PredSetOp,
    pub cmp_op: FloatCmpOp,

    #[src_type(F16v2)]
    pub srcs: [Src; 2],

    #[src_type(Pred)]
    pub accum: Src,

    pub ftz: bool,

    // When not set, each dsts get the result of each lanes.
    // When set, the first dst gets the result of both lanes (res0 && res1)
    // and the second dst gets the negation !(res0 && res1)
    // before applying the accumulator.
    pub horizontal: bool,
}

impl DisplayOp for OpHSetP2 {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let ftz = if self.ftz { ".ftz" } else { "" };
        write!(f, "hsetp2{}{ftz}", self.cmp_op)?;
        if !self.set_op.is_trivial(&self.accum) {
            write!(f, "{}", self.set_op)?;
        }
        write!(f, " {} {}", self.srcs[0], self.srcs[1])?;
        if !self.set_op.is_trivial(&self.accum) {
            write!(f, " {}", self.accum)?;
        }
        Ok(())
    }
}
impl_display_for_op!(OpHSetP2);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpHMul2 {
    #[dst_type(F16v2)]
    pub dst: Dst,

    #[src_type(F16v2)]
    pub srcs: [Src; 2],

    pub saturate: bool,
    pub ftz: bool,
    pub dnz: bool,
}

impl DisplayOp for OpHMul2 {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let sat = if self.saturate { ".sat" } else { "" };
        write!(f, "hmul2{sat}")?;
        if self.dnz {
            write!(f, ".dnz")?;
        } else if self.ftz {
            write!(f, ".ftz")?;
        }
        write!(f, " {} {}", self.srcs[0], self.srcs[1])
    }
}
impl_display_for_op!(OpHMul2);

#[derive(Clone, Copy, Eq, PartialEq)]
#[allow(dead_code)]
pub enum ImmaSize {
    M8N8K16,
    M8N8K32,
    M16N8K16,
    M16N8K32,
    M16N8K64,
}

impl fmt::Display for ImmaSize {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ImmaSize::M8N8K16 => write!(f, ".m8n8k16"),
            ImmaSize::M8N8K32 => write!(f, ".m8n8k32"),
            ImmaSize::M16N8K16 => write!(f, ".m16n8k16"),
            ImmaSize::M16N8K32 => write!(f, ".m16n8k32"),
            ImmaSize::M16N8K64 => write!(f, ".m16n8k64"),
        }
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpImma {
    #[dst_type(Vec)]
    pub dst: Dst,

    pub mat_size: ImmaSize,
    pub src_types: [IntType; 2],
    pub saturate: bool,

    #[src_type(SSA)]
    pub srcs: [Src; 3],
}

impl DisplayOp for OpImma {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let sat = if self.saturate { ".sat" } else { "" };
        write!(
            f,
            "imma{}{}{}{sat} {} {} {}",
            self.mat_size,
            self.src_types[0],
            self.src_types[1],
            self.srcs[0],
            self.srcs[1],
            self.srcs[2],
        )
    }
}

impl_display_for_op!(OpImma);

#[derive(Clone, Copy, Eq, PartialEq)]
#[allow(dead_code)]
pub enum HmmaSize {
    M16N8K16,
    M16N8K8,
    M16N8K4,
}

impl fmt::Display for HmmaSize {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            HmmaSize::M16N8K16 => write!(f, ".m16n8k16"),
            HmmaSize::M16N8K8 => write!(f, ".m16n8k8"),
            HmmaSize::M16N8K4 => write!(f, ".m16n8k4"),
        }
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpHmma {
    #[dst_type(Vec)]
    pub dst: Dst,

    pub mat_size: HmmaSize,
    pub src_type: FloatType,
    pub dst_type: FloatType,

    #[src_type(SSA)]
    pub srcs: [Src; 3],
}

impl DisplayOp for OpHmma {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "hmma{}{} {} {} {}",
            self.mat_size,
            self.dst_type,
            self.srcs[0],
            self.srcs[1],
            self.srcs[2],
        )
    }
}

impl_display_for_op!(OpHmma);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpHFma2 {
    #[dst_type(F16v2)]
    pub dst: Dst,

    #[src_type(F16v2)]
    pub srcs: [Src; 3],

    pub saturate: bool,
    pub ftz: bool,
    pub dnz: bool,
    pub f32: bool,
}

impl DisplayOp for OpHFma2 {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let sat = if self.saturate { ".sat" } else { "" };
        let f32 = if self.f32 { ".f32" } else { "" };
        write!(f, "hfma2{sat}{f32}")?;
        if self.dnz {
            write!(f, ".dnz")?;
        } else if self.ftz {
            write!(f, ".ftz")?;
        }
        write!(f, " {} {} {}", self.srcs[0], self.srcs[1], self.srcs[2])
    }
}
impl_display_for_op!(OpHFma2);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpHMnMx2 {
    #[dst_type(F16v2)]
    pub dst: Dst,

    #[src_type(F16v2)]
    pub srcs: [Src; 2],

    #[src_type(Pred)]
    pub min: Src,

    pub ftz: bool,
}

impl DisplayOp for OpHMnMx2 {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let ftz = if self.ftz { ".ftz" } else { "" };
        write!(
            f,
            "hmnmx2{ftz} {} {} {}",
            self.srcs[0], self.srcs[1], self.min
        )
    }
}
impl_display_for_op!(OpHMnMx2);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpBMsk {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(ALU)]
    pub pos: Src,

    #[src_type(ALU)]
    pub width: Src,

    pub wrap: bool,
}

impl DisplayOp for OpBMsk {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let wrap = if self.wrap { ".wrap" } else { ".clamp" };
        write!(f, "bmsk{} {} {}", wrap, self.pos, self.width)
    }
}
impl_display_for_op!(OpBMsk);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpBRev {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(ALU)]
    pub src: Src,
}

impl DisplayOp for OpBRev {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "brev {}", self.src)
    }
}
impl_display_for_op!(OpBRev);

/// Bitfield extract. Extracts all bits from `base` starting at `offset` into
/// `dst`.
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpBfe {
    /// Where to insert the bits.
    #[dst_type(GPR)]
    pub dst: Dst,

    /// The source of bits to extract.
    #[src_type(ALU)]
    pub base: Src,

    /// The range of bits to extract. This source is interpreted as four
    /// separate bytes, [b0, b1, b2, b3].
    ///
    /// b0 and b1: unused
    /// b2: the number of bits to extract.
    /// b3: the offset of the first bit to extract.
    ///
    /// This matches the way the hardware works.
    #[src_type(ALU)]
    pub range: Src,

    /// Whether the output is signed
    pub signed: bool,

    /// Whether to reverse the bits before inserting them into `dst`.
    pub reverse: bool,
}

impl DisplayOp for OpBfe {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "bfe")?;
        if self.signed {
            write!(f, ".s")?;
        }
        if self.reverse {
            write!(f, ".rev")?;
        }
        write!(f, " {} {}", self.base, self.range,)
    }
}
impl_display_for_op!(OpBfe);

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpFlo {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(ALU)]
    pub src: Src,

    pub signed: bool,
    pub return_shift_amount: bool,
}

impl Foldable for OpFlo {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let src = f.get_u32_src(self, &self.src);
        let leading = if self.signed && (src & 0x80000000) != 0 {
            (!src).leading_zeros()
        } else {
            src.leading_zeros()
        };
        let dst = if self.return_shift_amount {
            leading
        } else {
            31 - leading
        };
        f.set_u32_dst(self, &self.dst, dst);
    }
}

impl DisplayOp for OpFlo {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "flo")?;
        if self.return_shift_amount {
            write!(f, ".samt")?;
        }
        write!(f, " {}", self.src)
    }
}
impl_display_for_op!(OpFlo);

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpIAbs {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(ALU)]
    pub src: Src,
}

impl Foldable for OpIAbs {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let src = f.get_u32_src(self, &self.src);
        let dst = (src as i32).unsigned_abs();
        f.set_u32_dst(self, &self.dst, dst);
    }
}

impl DisplayOp for OpIAbs {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "iabs {}", self.src)
    }
}
impl_display_for_op!(OpIAbs);

/// Only used on SM50
#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpIAdd2 {
    #[dst_type(GPR)]
    pub dst: Dst,
    #[dst_type(Carry)]
    pub carry_out: Dst,

    #[src_type(I32)]
    pub srcs: [Src; 2],
}

impl Foldable for OpIAdd2 {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let srcs = [
            f.get_u32_src(self, &self.srcs[0]),
            f.get_u32_src(self, &self.srcs[1]),
        ];

        let mut sum = 0_u64;
        for i in 0..2 {
            if self.srcs[i].src_mod.is_ineg() {
                // This is a very literal interpretation of 2's compliment.
                // This is not -u64::from(src) or u64::from(-src).
                sum += u64::from(!srcs[i]) + 1;
            } else {
                sum += u64::from(srcs[i]);
            }
        }

        f.set_u32_dst(self, &self.dst, sum as u32);
        f.set_carry_dst(self, &self.carry_out, sum >= (1 << 32));
    }
}

impl DisplayOp for OpIAdd2 {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "iadd2 {} {}", self.srcs[0], self.srcs[1])
    }
}

/// Only used on SM50
#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpIAdd2X {
    #[dst_type(GPR)]
    pub dst: Dst,
    #[dst_type(Carry)]
    pub carry_out: Dst,

    #[src_type(B32)]
    pub srcs: [Src; 2],
    #[src_type(Carry)]
    pub carry_in: Src,
}

impl Foldable for OpIAdd2X {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let srcs = [
            f.get_u32_bnot_src(self, &self.srcs[0]),
            f.get_u32_bnot_src(self, &self.srcs[1]),
        ];
        let carry_in = f.get_carry_src(self, &self.carry_in);

        let sum = u64::from(srcs[0]) + u64::from(srcs[1]) + u64::from(carry_in);

        f.set_u32_dst(self, &self.dst, sum as u32);
        f.set_carry_dst(self, &self.carry_out, sum >= (1 << 32));
    }
}

impl DisplayOp for OpIAdd2X {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "iadd2.x {} {}", self.srcs[0], self.srcs[1])?;
        if !self.carry_in.is_zero() {
            write!(f, " {}", self.carry_in)?;
        }
        Ok(())
    }
}

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpIAdd3 {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[dst_type(Pred)]
    pub overflow: [Dst; 2],

    #[src_type(I32)]
    pub srcs: [Src; 3],
}

impl Foldable for OpIAdd3 {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let srcs = [
            f.get_u32_src(self, &self.srcs[0]),
            f.get_u32_src(self, &self.srcs[1]),
            f.get_u32_src(self, &self.srcs[2]),
        ];

        let mut sum = 0_u64;
        for i in 0..3 {
            if self.srcs[i].src_mod.is_ineg() {
                // This is a very literal interpretation of 2's compliment.
                // This is not -u64::from(src) or u64::from(-src).
                sum += u64::from(!srcs[i]) + 1;
            } else {
                sum += u64::from(srcs[i]);
            }
        }

        f.set_u32_dst(self, &self.dst, sum as u32);
        f.set_pred_dst(self, &self.overflow[0], sum >= 1_u64 << 32);
        f.set_pred_dst(self, &self.overflow[1], sum >= 2_u64 << 32);
    }
}

impl DisplayOp for OpIAdd3 {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "iadd3 {} {} {}",
            self.srcs[0], self.srcs[1], self.srcs[2],
        )
    }
}
impl_display_for_op!(OpIAdd3);

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpIAdd3X {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[dst_type(Pred)]
    pub overflow: [Dst; 2],

    #[src_type(B32)]
    pub srcs: [Src; 3],

    #[src_type(Pred)]
    pub carry: [Src; 2],
}

impl Foldable for OpIAdd3X {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let srcs = [
            f.get_u32_bnot_src(self, &self.srcs[0]),
            f.get_u32_bnot_src(self, &self.srcs[1]),
            f.get_u32_bnot_src(self, &self.srcs[2]),
        ];
        let carry = [
            f.get_pred_src(self, &self.carry[0]),
            f.get_pred_src(self, &self.carry[1]),
        ];

        let mut sum = 0_u64;
        for i in 0..3 {
            sum += u64::from(srcs[i]);
        }

        for i in 0..2 {
            sum += u64::from(carry[i]);
        }

        f.set_u32_dst(self, &self.dst, sum as u32);
        f.set_pred_dst(self, &self.overflow[0], sum >= 1_u64 << 32);
        f.set_pred_dst(self, &self.overflow[1], sum >= 2_u64 << 32);
    }
}

impl DisplayOp for OpIAdd3X {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "iadd3.x {} {} {} {} {}",
            self.srcs[0],
            self.srcs[1],
            self.srcs[2],
            self.carry[0],
            self.carry[1]
        )
    }
}
impl_display_for_op!(OpIAdd3X);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpIDp4 {
    #[dst_type(GPR)]
    pub dst: Dst,

    pub src_types: [IntType; 2],

    #[src_type(I32)]
    pub srcs: [Src; 3],
}

impl DisplayOp for OpIDp4 {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "idp4{}{} {} {} {}",
            self.src_types[0],
            self.src_types[1],
            self.srcs[0],
            self.srcs[1],
            self.srcs[2],
        )
    }
}
impl_display_for_op!(OpIDp4);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpIMad {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(ALU)]
    pub srcs: [Src; 3],

    pub signed: bool,
}

impl DisplayOp for OpIMad {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "imad {} {} {}", self.srcs[0], self.srcs[1], self.srcs[2],)
    }
}
impl_display_for_op!(OpIMad);

/// Only used on SM50
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpIMul {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(ALU)]
    pub srcs: [Src; 2],

    pub signed: [bool; 2],
    pub high: bool,
}

impl DisplayOp for OpIMul {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "imul")?;
        if self.high {
            write!(f, ".hi")?;
        }
        let src_type = |signed| if signed { ".s32" } else { ".u32" };
        write!(
            f,
            "{}{}",
            src_type(self.signed[0]),
            src_type(self.signed[1])
        )?;
        write!(f, " {} {}", self.srcs[0], self.srcs[1])
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpIMad64 {
    #[dst_type(Vec)]
    pub dst: Dst,

    #[src_type(ALU)]
    pub srcs: [Src; 3],

    pub signed: bool,
}

impl DisplayOp for OpIMad64 {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "imad64 {} {} {}",
            self.srcs[0], self.srcs[1], self.srcs[2],
        )
    }
}
impl_display_for_op!(OpIMad64);

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpIMnMx {
    #[dst_type(GPR)]
    pub dst: Dst,

    pub cmp_type: IntCmpType,

    #[src_type(ALU)]
    pub srcs: [Src; 2],

    #[src_type(Pred)]
    pub min: Src,
}

impl Foldable for OpIMnMx {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let (a, b) = (
            f.get_u32_bnot_src(self, &self.srcs[0]),
            f.get_u32_bnot_src(self, &self.srcs[1]),
        );
        let min = f.get_pred_src(self, &self.min);

        let res = match (min, self.cmp_type) {
            (true, IntCmpType::U32) => a.min(b),
            (true, IntCmpType::I32) => (a as i32).min(b as i32) as u32,
            (false, IntCmpType::U32) => a.max(b),
            (false, IntCmpType::I32) => (a as i32).max(b as i32) as u32,
        };

        f.set_u32_dst(self, &self.dst, res);
    }
}

impl DisplayOp for OpIMnMx {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "imnmx{} {} {} {}",
            self.cmp_type, self.srcs[0], self.srcs[1], self.min
        )
    }
}
impl_display_for_op!(OpIMnMx);

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpISetP {
    #[dst_type(Pred)]
    pub dst: Dst,

    pub set_op: PredSetOp,
    pub cmp_op: IntCmpOp,
    pub cmp_type: IntCmpType,
    pub ex: bool,

    #[src_type(ALU)]
    pub srcs: [Src; 2],

    #[src_type(Pred)]
    pub accum: Src,

    #[src_type(Pred)]
    pub low_cmp: Src,
}

impl Foldable for OpISetP {
    fn fold(&self, sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let x = f.get_u32_src(self, &self.srcs[0]);
        let y = f.get_u32_src(self, &self.srcs[1]);
        let accum = f.get_pred_src(self, &self.accum);
        let low_cmp = f.get_pred_src(self, &self.low_cmp);

        let cmp = if self.cmp_type.is_signed() {
            let x = x as i32;
            let y = y as i32;
            match &self.cmp_op {
                IntCmpOp::False => false,
                IntCmpOp::True => true,
                IntCmpOp::Eq => x == y,
                IntCmpOp::Ne => x != y,
                IntCmpOp::Lt => x < y,
                IntCmpOp::Le => x <= y,
                IntCmpOp::Gt => x > y,
                IntCmpOp::Ge => x >= y,
            }
        } else {
            match &self.cmp_op {
                IntCmpOp::False => false,
                IntCmpOp::True => true,
                IntCmpOp::Eq => x == y,
                IntCmpOp::Ne => x != y,
                IntCmpOp::Lt => x < y,
                IntCmpOp::Le => x <= y,
                IntCmpOp::Gt => x > y,
                IntCmpOp::Ge => x >= y,
            }
        };

        let cmp_op_is_const =
            matches!(self.cmp_op, IntCmpOp::False | IntCmpOp::True);
        let cmp = if self.ex && x == y && !cmp_op_is_const {
            // Pre-Volta, isetp.x takes the accumulator into account.  If we
            // want to support this, we need to take an an accumulator into
            // account.  Disallow it for now.
            assert!(sm.sm() >= 70);
            low_cmp
        } else {
            cmp
        };

        let dst = self.set_op.eval(cmp, accum);

        f.set_pred_dst(self, &self.dst, dst);
    }
}

impl DisplayOp for OpISetP {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "isetp{}{}", self.cmp_op, self.cmp_type)?;
        if !self.set_op.is_trivial(&self.accum) {
            write!(f, "{}", self.set_op)?;
        }
        if self.ex {
            write!(f, ".ex")?;
        }
        write!(f, " {} {}", self.srcs[0], self.srcs[1])?;
        if !self.set_op.is_trivial(&self.accum) {
            write!(f, " {}", self.accum)?;
        }
        if self.ex {
            write!(f, " {}", self.low_cmp)?;
        }
        Ok(())
    }
}
impl_display_for_op!(OpISetP);

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpLea {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[dst_type(Pred)]
    pub overflow: Dst,

    #[src_type(ALU)]
    pub a: Src,

    #[src_type(I32)]
    pub b: Src,

    #[src_type(ALU)]
    pub a_high: Src, // High 32-bits of a if .dst_high is set

    pub shift: u8,
    pub dst_high: bool,
    pub intermediate_mod: SrcMod, // Modifier for shifted temporary (a << shift)
}

impl Foldable for OpLea {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let a = f.get_u32_src(self, &self.a);
        let mut b = f.get_u32_src(self, &self.b);
        let a_high = f.get_u32_src(self, &self.a_high);

        let mut overflow = false;

        let mut shift_result = if self.dst_high {
            let a = a as u64;
            let a_high = a_high as u64;
            let a = (a_high << 32) | a;

            (a >> (32 - self.shift)) as u32
        } else {
            a << self.shift
        };

        if self.intermediate_mod.is_ineg() {
            let o;
            (shift_result, o) = u32::overflowing_add(!shift_result, 1);
            overflow |= o;
        }

        if self.b.src_mod.is_ineg() {
            let o;
            (b, o) = u32::overflowing_add(!b, 1);
            overflow |= o;
        }

        let (dst, o) = u32::overflowing_add(shift_result, b);
        overflow |= o;

        f.set_u32_dst(self, &self.dst, dst);
        f.set_pred_dst(self, &self.overflow, overflow);
    }
}

impl DisplayOp for OpLea {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "lea")?;
        if self.dst_high {
            write!(f, ".hi")?;
        }
        write!(f, " {} {} {}", self.a, self.shift, self.b)?;
        if self.dst_high {
            write!(f, " {}", self.a_high)?;
        }
        Ok(())
    }
}
impl_display_for_op!(OpLea);

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpLeaX {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[dst_type(Pred)]
    pub overflow: Dst,

    #[src_type(ALU)]
    pub a: Src,

    #[src_type(B32)]
    pub b: Src,

    #[src_type(ALU)]
    pub a_high: Src, // High 32-bits of a if .dst_high is set

    #[src_type(Pred)]
    pub carry: Src,

    pub shift: u8,
    pub dst_high: bool,
    pub intermediate_mod: SrcMod, // Modifier for shifted temporary (a << shift)
}

impl Foldable for OpLeaX {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let a = f.get_u32_src(self, &self.a);
        let mut b = f.get_u32_src(self, &self.b);
        let a_high = f.get_u32_src(self, &self.a_high);
        let carry = f.get_pred_src(self, &self.carry);

        let mut overflow = false;

        let mut shift_result = if self.dst_high {
            let a = a as u64;
            let a_high = a_high as u64;
            let a = (a_high << 32) | a;

            (a >> (32 - self.shift)) as u32
        } else {
            a << self.shift
        };

        if self.intermediate_mod.is_bnot() {
            shift_result = !shift_result;
        }

        if self.b.src_mod.is_bnot() {
            b = !b;
        }

        let (dst, o) = u32::overflowing_add(shift_result, b);
        overflow |= o;

        let (dst, o) = u32::overflowing_add(dst, if carry { 1 } else { 0 });
        overflow |= o;

        f.set_u32_dst(self, &self.dst, dst);
        f.set_pred_dst(self, &self.overflow, overflow);
    }
}

impl DisplayOp for OpLeaX {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "lea.x")?;
        if self.dst_high {
            write!(f, ".hi")?;
        }
        write!(f, " {} {} {}", self.a, self.shift, self.b)?;
        if self.dst_high {
            write!(f, " {}", self.a_high)?;
        }
        write!(f, " {}", self.carry)?;
        Ok(())
    }
}
impl_display_for_op!(OpLeaX);

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpLop2 {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(B32)]
    pub srcs: [Src; 2],

    pub op: LogicOp2,
}

impl DisplayOp for OpLop2 {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "lop2.{} {} {}", self.op, self.srcs[0], self.srcs[1],)
    }
}

impl Foldable for OpLop2 {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let srcs = [
            f.get_u32_bnot_src(self, &self.srcs[0]),
            f.get_u32_bnot_src(self, &self.srcs[1]),
        ];
        let dst = match self.op {
            LogicOp2::And => srcs[0] & srcs[1],
            LogicOp2::Or => srcs[0] | srcs[1],
            LogicOp2::Xor => srcs[0] ^ srcs[1],
            LogicOp2::PassB => srcs[1],
        };
        f.set_u32_dst(self, &self.dst, dst);
    }
}

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpLop3 {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(ALU)]
    pub srcs: [Src; 3],

    pub op: LogicOp3,
}

impl Foldable for OpLop3 {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let srcs = [
            f.get_u32_bnot_src(self, &self.srcs[0]),
            f.get_u32_bnot_src(self, &self.srcs[1]),
            f.get_u32_bnot_src(self, &self.srcs[2]),
        ];
        let dst = self.op.eval(srcs[0], srcs[1], srcs[2]);
        f.set_u32_dst(self, &self.dst, dst);
    }
}

impl DisplayOp for OpLop3 {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "lop3.{} {} {} {}",
            self.op, self.srcs[0], self.srcs[1], self.srcs[2],
        )
    }
}
impl_display_for_op!(OpLop3);

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum ShflOp {
    Idx,
    Up,
    Down,
    Bfly,
}

impl fmt::Display for ShflOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ShflOp::Idx => write!(f, "idx"),
            ShflOp::Up => write!(f, "up"),
            ShflOp::Down => write!(f, "down"),
            ShflOp::Bfly => write!(f, "bfly"),
        }
    }
}

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpShf {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(GPR)]
    pub low: Src,

    #[src_type(ALU)]
    pub high: Src,

    #[src_type(ALU)]
    pub shift: Src,

    pub right: bool,
    pub wrap: bool,
    pub data_type: IntType,
    pub dst_high: bool,
}

fn reduce_shift_imm(shift: &mut Src, wrap: bool, bits: u32) {
    debug_assert!(shift.src_mod.is_none());
    if let SrcRef::Imm32(shift) = &mut shift.src_ref {
        if wrap {
            *shift = *shift & (bits - 1);
        } else {
            *shift = std::cmp::min(*shift, bits)
        }
    }
}

impl OpShf {
    /// Reduces the shift immediate, if any.  Out-of-range shifts are either
    /// clamped to the maximum or wrapped as needed.
    pub fn reduce_shift_imm(&mut self) {
        let bits = self.data_type.bits().try_into().unwrap();
        reduce_shift_imm(&mut self.shift, self.wrap, bits);
    }
}

impl Foldable for OpShf {
    fn fold(&self, sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let low = f.get_u32_src(self, &self.low);
        let high = f.get_u32_src(self, &self.high);
        let shift = f.get_u32_src(self, &self.shift);

        let bits: u32 = self.data_type.bits().try_into().unwrap();
        let shift = if self.wrap {
            shift & (bits - 1)
        } else {
            min(shift, bits)
        };

        let x = u64::from(low) | (u64::from(high) << 32);
        let shifted = if sm.sm() < 70
            && self.dst_high
            && self.data_type != IntType::I64
        {
            if self.right {
                x.checked_shr(shift).unwrap_or(0)
            } else {
                x.checked_shl(shift).unwrap_or(0)
            }
        } else if self.data_type.is_signed() {
            if self.right {
                let x = x as i64;
                x.checked_shr(shift).unwrap_or(x >> 63) as u64
            } else {
                x.checked_shl(shift).unwrap_or(0)
            }
        } else {
            if self.right {
                x.checked_shr(shift).unwrap_or(0)
            } else {
                x.checked_shl(shift).unwrap_or(0)
            }
        };

        let dst = if (sm.sm() < 70 && !self.right) || self.dst_high {
            (shifted >> 32) as u32
        } else {
            shifted as u32
        };

        f.set_u32_dst(self, &self.dst, dst);
    }
}

impl DisplayOp for OpShf {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "shf")?;
        if self.right {
            write!(f, ".r")?;
        } else {
            write!(f, ".l")?;
        }
        if self.wrap {
            write!(f, ".w")?;
        }
        write!(f, "{}", self.data_type)?;
        if self.dst_high {
            write!(f, ".hi")?;
        }
        write!(f, " {} {} {}", self.low, self.high, self.shift)
    }
}
impl_display_for_op!(OpShf);

/// Only used on SM50
#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpShl {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(GPR)]
    pub src: Src,

    #[src_type(ALU)]
    pub shift: Src,

    pub wrap: bool,
}

impl OpShl {
    /// Reduces the shift immediate, if any.  Out-of-range shifts are either
    /// clamped to the maximum or wrapped as needed.
    pub fn reduce_shift_imm(&mut self) {
        reduce_shift_imm(&mut self.shift, self.wrap, 32);
    }
}

impl DisplayOp for OpShl {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "shl")?;
        if self.wrap {
            write!(f, ".w")?;
        }
        write!(f, " {} {}", self.src, self.shift)
    }
}

impl Foldable for OpShl {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let x = f.get_u32_src(self, &self.src);
        let shift = f.get_u32_src(self, &self.shift);

        let shift = if self.wrap {
            shift & 31
        } else {
            min(shift, 32)
        };
        let dst = x.checked_shl(shift).unwrap_or(0);
        f.set_u32_dst(self, &self.dst, dst);
    }
}

/// Only used on SM50
#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpShr {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(GPR)]
    pub src: Src,

    #[src_type(ALU)]
    pub shift: Src,

    pub wrap: bool,
    pub signed: bool,
}

impl DisplayOp for OpShr {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "shr")?;
        if self.wrap {
            write!(f, ".w")?;
        }
        if !self.signed {
            write!(f, ".u32")?;
        }
        write!(f, " {} {}", self.src, self.shift)
    }
}

impl OpShr {
    /// Reduces the shift immediate, if any.  Out-of-range shifts are either
    /// clamped to the maximum or wrapped as needed.
    pub fn reduce_shift_imm(&mut self) {
        reduce_shift_imm(&mut self.shift, self.wrap, 32);
    }
}

impl Foldable for OpShr {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let x = f.get_u32_src(self, &self.src);
        let shift = f.get_u32_src(self, &self.shift);

        let shift = if self.wrap {
            shift & 31
        } else {
            min(shift, 32)
        };
        let dst = if self.signed {
            let x = x as i32;
            x.checked_shr(shift).unwrap_or(x >> 31) as u32
        } else {
            x.checked_shr(shift).unwrap_or(0)
        };
        f.set_u32_dst(self, &self.dst, dst);
    }
}

#[repr(C)]
pub struct OpF2F {
    pub dst: Dst,
    pub src: Src,

    pub src_type: FloatType,
    pub dst_type: FloatType,
    pub rnd_mode: FRndMode,
    pub ftz: bool,
    /// For 16-bit up-conversions, take the high 16 bits of the source register.
    /// For 16-bit down-conversions, place the result into the upper 16 bits of
    /// the destination register
    pub high: bool,
    /// Round to the nearest integer rather than nearest float
    ///
    /// Not available on SM70+
    pub integer_rnd: bool,
}

impl AsSlice<Src> for OpF2F {
    type Attr = SrcType;

    fn as_slice(&self) -> &[Src] {
        std::slice::from_ref(&self.src)
    }

    fn as_mut_slice(&mut self) -> &mut [Src] {
        std::slice::from_mut(&mut self.src)
    }

    fn attrs(&self) -> SrcTypeList {
        let src_type = match self.src_type {
            FloatType::F16 => SrcType::F16,
            FloatType::F32 => SrcType::F32,
            FloatType::F64 => SrcType::F64,
        };
        SrcTypeList::Uniform(src_type)
    }
}

impl AsSlice<Dst> for OpF2F {
    type Attr = DstType;

    fn as_slice(&self) -> &[Dst] {
        std::slice::from_ref(&self.dst)
    }

    fn as_mut_slice(&mut self) -> &mut [Dst] {
        std::slice::from_mut(&mut self.dst)
    }

    fn attrs(&self) -> DstTypeList {
        let dst_type = match self.dst_type {
            FloatType::F16 => DstType::F16,
            FloatType::F32 => DstType::F32,
            FloatType::F64 => DstType::F64,
        };
        DstTypeList::Uniform(dst_type)
    }
}

impl DisplayOp for OpF2F {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "f2f")?;
        if self.ftz {
            write!(f, ".ftz")?;
        }
        if self.integer_rnd {
            write!(f, ".int")?;
        }
        write!(
            f,
            "{}{}{} {}",
            self.dst_type, self.src_type, self.rnd_mode, self.src,
        )
    }
}
impl_display_for_op!(OpF2F);

#[repr(C)]
#[derive(DstsAsSlice, SrcsAsSlice)]
pub struct OpF2FP {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(ALU)]
    pub srcs: [Src; 2],

    pub rnd_mode: FRndMode,
}

impl DisplayOp for OpF2FP {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "f2fp.pack_ab")?;
        if self.rnd_mode != FRndMode::NearestEven {
            write!(f, "{}", self.rnd_mode)?;
        }
        write!(f, " {}, {}", self.srcs[0], self.srcs[1],)
    }
}
impl_display_for_op!(OpF2FP);

#[repr(C)]
#[derive(DstsAsSlice)]
pub struct OpF2I {
    #[dst_type(GPR)]
    pub dst: Dst,

    pub src: Src,

    pub src_type: FloatType,
    pub dst_type: IntType,
    pub rnd_mode: FRndMode,
    pub ftz: bool,
}

impl AsSlice<Src> for OpF2I {
    type Attr = SrcType;

    fn as_slice(&self) -> &[Src] {
        std::slice::from_ref(&self.src)
    }

    fn as_mut_slice(&mut self) -> &mut [Src] {
        std::slice::from_mut(&mut self.src)
    }

    fn attrs(&self) -> SrcTypeList {
        let src_type = match self.src_type {
            FloatType::F16 => SrcType::F16,
            FloatType::F32 => SrcType::F32,
            FloatType::F64 => SrcType::F64,
        };
        SrcTypeList::Uniform(src_type)
    }
}

impl DisplayOp for OpF2I {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let ftz = if self.ftz { ".ftz" } else { "" };
        write!(
            f,
            "f2i{}{}{}{ftz} {}",
            self.dst_type, self.src_type, self.rnd_mode, self.src,
        )
    }
}
impl_display_for_op!(OpF2I);

#[repr(C)]
pub struct OpI2F {
    pub dst: Dst,
    pub src: Src,

    pub dst_type: FloatType,
    pub src_type: IntType,
    pub rnd_mode: FRndMode,
}

impl AsSlice<Src> for OpI2F {
    type Attr = SrcType;

    fn as_slice(&self) -> &[Src] {
        std::slice::from_ref(&self.src)
    }

    fn as_mut_slice(&mut self) -> &mut [Src] {
        std::slice::from_mut(&mut self.src)
    }

    fn attrs(&self) -> SrcTypeList {
        if self.src_type.bits() <= 32 {
            SrcTypeList::Uniform(SrcType::ALU)
        } else {
            SrcTypeList::Uniform(SrcType::GPR)
        }
    }
}

impl AsSlice<Dst> for OpI2F {
    type Attr = DstType;

    fn as_slice(&self) -> &[Dst] {
        std::slice::from_ref(&self.dst)
    }

    fn as_mut_slice(&mut self) -> &mut [Dst] {
        std::slice::from_mut(&mut self.dst)
    }

    fn attrs(&self) -> DstTypeList {
        let dst_type = match self.dst_type {
            FloatType::F16 => DstType::F16,
            FloatType::F32 => DstType::F32,
            FloatType::F64 => DstType::F64,
        };
        DstTypeList::Uniform(dst_type)
    }
}

impl DisplayOp for OpI2F {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "i2f{}{}{} {}",
            self.dst_type, self.src_type, self.rnd_mode, self.src,
        )
    }
}
impl_display_for_op!(OpI2F);

/// Not used on SM70+
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpI2I {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(ALU)]
    pub src: Src,

    pub src_type: IntType,
    pub dst_type: IntType,

    pub saturate: bool,
    pub abs: bool,
    pub neg: bool,
}

impl DisplayOp for OpI2I {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "i2i")?;
        if self.saturate {
            write!(f, ".sat ")?;
        }
        write!(f, "{}{} {}", self.dst_type, self.src_type, self.src,)?;
        if self.abs {
            write!(f, ".abs")?;
        }
        if self.neg {
            write!(f, ".neg")?;
        }
        Ok(())
    }
}
impl_display_for_op!(OpI2I);

#[repr(C)]
#[derive(DstsAsSlice)]
pub struct OpFRnd {
    #[dst_type(F32)]
    pub dst: Dst,

    pub src: Src,

    pub dst_type: FloatType,
    pub src_type: FloatType,
    pub rnd_mode: FRndMode,
    pub ftz: bool,
}

impl AsSlice<Src> for OpFRnd {
    type Attr = SrcType;

    fn as_slice(&self) -> &[Src] {
        std::slice::from_ref(&self.src)
    }

    fn as_mut_slice(&mut self) -> &mut [Src] {
        std::slice::from_mut(&mut self.src)
    }

    fn attrs(&self) -> SrcTypeList {
        let src_type = match self.src_type {
            FloatType::F16 => SrcType::F16,
            FloatType::F32 => SrcType::F32,
            FloatType::F64 => SrcType::F64,
        };
        SrcTypeList::Uniform(src_type)
    }
}

impl DisplayOp for OpFRnd {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let ftz = if self.ftz { ".ftz" } else { "" };
        write!(
            f,
            "frnd{}{}{}{ftz} {}",
            self.dst_type, self.src_type, self.rnd_mode, self.src,
        )
    }
}
impl_display_for_op!(OpFRnd);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpMov {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(ALU)]
    pub src: Src,

    pub quad_lanes: u8,
}

impl DisplayOp for OpMov {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.quad_lanes == 0xf {
            write!(f, "mov {}", self.src)
        } else {
            write!(f, "mov[{:#x}] {}", self.quad_lanes, self.src)
        }
    }
}
impl_display_for_op!(OpMov);

#[derive(Copy, Clone)]
pub struct PrmtSelByte(u8);

impl PrmtSelByte {
    pub const INVALID: PrmtSelByte = PrmtSelByte(u8::MAX);

    pub fn new(src_idx: usize, byte_idx: usize, msb: bool) -> PrmtSelByte {
        assert!(src_idx < 2);
        assert!(byte_idx < 4);

        let mut nib = 0;
        nib |= (src_idx as u8) << 2;
        nib |= byte_idx as u8;
        if msb {
            nib |= 0x8;
        }
        PrmtSelByte(nib)
    }

    pub fn src(&self) -> usize {
        ((self.0 >> 2) & 0x1).into()
    }

    pub fn byte(&self) -> usize {
        (self.0 & 0x3).into()
    }

    pub fn msb(&self) -> bool {
        (self.0 & 0x8) != 0
    }

    pub fn fold_u32(&self, u: u32) -> u8 {
        let mut sb = (u >> (self.byte() * 8)) as u8;
        if self.msb() {
            sb = ((sb as i8) >> 7) as u8;
        }
        sb
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub struct PrmtSel(pub u16);

impl PrmtSel {
    pub fn new(bytes: [PrmtSelByte; 4]) -> PrmtSel {
        let mut sel = 0;
        for i in 0..4 {
            assert!(bytes[i].0 <= 0xf);
            sel |= u16::from(bytes[i].0) << (i * 4);
        }
        PrmtSel(sel)
    }

    pub fn get(&self, byte_idx: usize) -> PrmtSelByte {
        assert!(byte_idx < 4);
        PrmtSelByte(((self.0 >> (byte_idx * 4)) & 0xf) as u8)
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum PrmtMode {
    Index,
    Forward4Extract,
    Backward4Extract,
    Replicate8,
    EdgeClampLeft,
    EdgeClampRight,
    Replicate16,
}

impl fmt::Display for PrmtMode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PrmtMode::Index => Ok(()),
            PrmtMode::Forward4Extract => write!(f, ".f4e"),
            PrmtMode::Backward4Extract => write!(f, ".b4e"),
            PrmtMode::Replicate8 => write!(f, ".rc8"),
            PrmtMode::EdgeClampLeft => write!(f, ".ecl"),
            PrmtMode::EdgeClampRight => write!(f, ".ecl"),
            PrmtMode::Replicate16 => write!(f, ".rc16"),
        }
    }
}

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
/// Permutes `srcs` into `dst` using `selection`.
pub struct OpPrmt {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(ALU)]
    pub srcs: [Src; 2],

    #[src_type(ALU)]
    pub sel: Src,

    pub mode: PrmtMode,
}

impl OpPrmt {
    pub fn get_sel(&self) -> Option<PrmtSel> {
        // TODO: We could construct a PrmtSel for the other modes but we don't
        // use them right now because they're kinda pointless.
        if self.mode != PrmtMode::Index {
            return None;
        }

        self.sel.as_u32(SrcType::ALU).map(|sel| {
            // The top 16 bits are ignored
            PrmtSel(sel as u16)
        })
    }

    /// Reduces the sel immediate, if any.
    pub fn reduce_sel_imm(&mut self) {
        assert!(self.sel.src_mod.is_none());
        if let SrcRef::Imm32(sel) = &mut self.sel.src_ref {
            // Only the bottom 16 bits matter anyway
            *sel &= 0xffff;
        }
    }

    pub fn as_u32(&self) -> Option<u32> {
        let sel = self.get_sel()?;

        let mut imm = 0_u32;
        for b in 0..4 {
            let sel_byte = sel.get(b);
            let src_u32 = self.srcs[sel_byte.src()].as_u32(SrcType::ALU)?;

            let sb = sel_byte.fold_u32(src_u32);
            imm |= u32::from(sb) << (b * 8);
        }
        Some(imm)
    }
}

impl Foldable for OpPrmt {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let srcs = [
            f.get_u32_src(self, &self.srcs[0]),
            f.get_u32_src(self, &self.srcs[1]),
        ];
        let sel = f.get_u32_src(self, &self.sel);

        assert!(self.mode == PrmtMode::Index);
        let sel = PrmtSel(sel as u16);

        let mut dst = 0_u32;
        for b in 0..4 {
            let sel_byte = sel.get(b);
            let src = srcs[sel_byte.src()];
            let sb = sel_byte.fold_u32(src);
            dst |= u32::from(sb) << (b * 8);
        }

        f.set_u32_dst(self, &self.dst, dst);
    }
}

impl DisplayOp for OpPrmt {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "prmt{} {} [{}] {}",
            self.mode, self.srcs[0], self.sel, self.srcs[1],
        )
    }
}
impl_display_for_op!(OpPrmt);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSel {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(Pred)]
    pub cond: Src,

    #[src_type(ALU)]
    pub srcs: [Src; 2],
}

impl DisplayOp for OpSel {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "sel {} {} {}", self.cond, self.srcs[0], self.srcs[1],)
    }
}
impl_display_for_op!(OpSel);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpShfl {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[dst_type(Pred)]
    pub in_bounds: Dst,

    #[src_type(SSA)]
    pub src: Src,

    #[src_type(ALU)]
    pub lane: Src,

    #[src_type(ALU)]
    pub c: Src,

    pub op: ShflOp,
}

impl OpShfl {
    /// Reduces the lane and c immediates, if any.  The hardware only uses
    /// some of the bits of `lane` and `c` and ignores the rest.  This method
    /// masks off the unused bits and ensures that any immediate values fit
    /// in the limited encoding space in the instruction.
    pub fn reduce_lane_c_imm(&mut self) {
        debug_assert!(self.lane.src_mod.is_none());
        if let SrcRef::Imm32(lane) = &mut self.lane.src_ref {
            *lane &= 0x1f;
        }

        debug_assert!(self.c.src_mod.is_none());
        if let SrcRef::Imm32(c) = &mut self.c.src_ref {
            *c &= 0x1f1f;
        }
    }
}

impl DisplayOp for OpShfl {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "shfl.{} {} {} {}", self.op, self.src, self.lane, self.c)
    }
}
impl_display_for_op!(OpShfl);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpPLop3 {
    #[dst_type(Pred)]
    pub dsts: [Dst; 2],

    #[src_type(Pred)]
    pub srcs: [Src; 3],

    pub ops: [LogicOp3; 2],
}

impl DisplayOp for OpPLop3 {
    fn fmt_dsts(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} {}", self.dsts[0], self.dsts[1])
    }

    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "plop3 {} {} {} {} {}",
            self.srcs[0], self.srcs[1], self.srcs[2], self.ops[0], self.ops[1],
        )
    }
}
impl_display_for_op!(OpPLop3);

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpPSetP {
    #[dst_type(Pred)]
    pub dsts: [Dst; 2],

    pub ops: [PredSetOp; 2],

    #[src_type(Pred)]
    pub srcs: [Src; 3],
}

impl Foldable for OpPSetP {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let srcs = [
            f.get_pred_src(self, &self.srcs[0]),
            f.get_pred_src(self, &self.srcs[1]),
            f.get_pred_src(self, &self.srcs[2]),
        ];

        let tmp = self.ops[0].eval(srcs[0], srcs[1]);
        let dst0 = self.ops[1].eval(srcs[2], tmp);

        let tmp = self.ops[0].eval(!srcs[0], srcs[1]);
        let dst1 = self.ops[1].eval(srcs[2], tmp);

        f.set_pred_dst(self, &self.dsts[0], dst0);
        f.set_pred_dst(self, &self.dsts[1], dst1);
    }
}

impl DisplayOp for OpPSetP {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "psetp{}{} {} {} {}",
            self.ops[0], self.ops[1], self.srcs[0], self.srcs[1], self.srcs[2],
        )
    }
}

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpPopC {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(B32)]
    pub src: Src,
}

impl Foldable for OpPopC {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let src = f.get_u32_bnot_src(self, &self.src);
        let dst = src.count_ones();
        f.set_u32_dst(self, &self.dst, dst);
    }
}

impl DisplayOp for OpPopC {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "popc {}", self.src,)
    }
}
impl_display_for_op!(OpPopC);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpR2UR {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(GPR)]
    pub src: Src,
}

impl DisplayOp for OpR2UR {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "r2ur {}", self.src)
    }
}
impl_display_for_op!(OpR2UR);

#[derive(Copy, Clone, PartialEq, Eq)]
pub enum ReduxOp {
    And,
    Or,
    Xor,
    Sum,
    Min(IntCmpType),
    Max(IntCmpType),
}

impl fmt::Display for ReduxOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ReduxOp::And => write!(f, ".and"),
            ReduxOp::Or => write!(f, ".or"),
            ReduxOp::Xor => write!(f, ".xor"),
            ReduxOp::Sum => write!(f, ".sum"),
            ReduxOp::Min(cmp) => write!(f, ".min{cmp}"),
            ReduxOp::Max(cmp) => write!(f, ".max{cmp}"),
        }
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpRedux {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(GPR)]
    pub src: Src,

    pub op: ReduxOp,
}

impl DisplayOp for OpRedux {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "redux{} {}", self.op, self.src)
    }
}
impl_display_for_op!(OpRedux);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpTex {
    pub dsts: [Dst; 2],
    pub fault: Dst,

    pub tex: TexRef,

    #[src_type(SSA)]
    pub srcs: [Src; 2],

    pub dim: TexDim,
    pub lod_mode: TexLodMode,
    pub deriv_mode: TexDerivMode,
    pub z_cmpr: bool,
    pub offset_mode: TexOffsetMode,
    pub mem_eviction_priority: MemEvictionPriority,
    pub nodep: bool,
    pub channel_mask: ChannelMask,
}

impl DisplayOp for OpTex {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "tex{}{}{}{}",
            self.dim, self.lod_mode, self.offset_mode, self.deriv_mode
        )?;
        if self.z_cmpr {
            write!(f, ".dc")?;
        }
        write!(f, "{}", self.mem_eviction_priority)?;
        if self.nodep {
            write!(f, ".nodep")?;
        }
        write!(f, "{}", self.channel_mask)?;
        write!(f, " {} {} {}", self.tex, self.srcs[0], self.srcs[1])
    }
}
impl_display_for_op!(OpTex);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpTld {
    pub dsts: [Dst; 2],
    pub fault: Dst,

    pub tex: TexRef,

    #[src_type(SSA)]
    pub srcs: [Src; 2],

    pub dim: TexDim,
    pub is_ms: bool,
    pub lod_mode: TexLodMode,
    pub offset_mode: TexOffsetMode,
    pub mem_eviction_priority: MemEvictionPriority,
    pub nodep: bool,
    pub channel_mask: ChannelMask,
}

impl DisplayOp for OpTld {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "tld{}{}{}", self.dim, self.lod_mode, self.offset_mode)?;
        if self.is_ms {
            write!(f, ".ms")?;
        }
        write!(f, "{}", self.mem_eviction_priority)?;
        if self.nodep {
            write!(f, ".nodep")?;
        }
        write!(f, "{}", self.channel_mask)?;
        write!(f, " {} {} {}", self.tex, self.srcs[0], self.srcs[1])
    }
}
impl_display_for_op!(OpTld);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpTld4 {
    pub dsts: [Dst; 2],
    pub fault: Dst,

    pub tex: TexRef,

    #[src_type(SSA)]
    pub srcs: [Src; 2],

    pub dim: TexDim,
    pub comp: u8,
    pub offset_mode: TexOffsetMode,
    pub z_cmpr: bool,
    pub mem_eviction_priority: MemEvictionPriority,
    pub nodep: bool,
    pub channel_mask: ChannelMask,
}

impl DisplayOp for OpTld4 {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "tld4.g{}{}", self.dim, self.offset_mode)?;
        if self.z_cmpr {
            write!(f, ".dc")?;
        }
        write!(f, "{}", self.mem_eviction_priority)?;
        if self.nodep {
            write!(f, ".nodep")?;
        }
        write!(f, "{}", self.channel_mask)?;
        write!(f, " {} {} {}", self.tex, self.srcs[0], self.srcs[1])
    }
}
impl_display_for_op!(OpTld4);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpTmml {
    pub dsts: [Dst; 2],

    pub tex: TexRef,

    #[src_type(SSA)]
    pub srcs: [Src; 2],

    pub dim: TexDim,
    pub deriv_mode: TexDerivMode,
    pub nodep: bool,
    pub channel_mask: ChannelMask,
}

impl DisplayOp for OpTmml {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "tmml.lod{}{}", self.dim, self.deriv_mode)?;
        if self.nodep {
            write!(f, ".nodep")?;
        }
        write!(f, "{}", self.channel_mask)?;
        write!(f, " {} {} {}", self.tex, self.srcs[0], self.srcs[1])
    }
}
impl_display_for_op!(OpTmml);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpTxd {
    pub dsts: [Dst; 2],
    pub fault: Dst,

    pub tex: TexRef,

    #[src_type(SSA)]
    pub srcs: [Src; 2],

    pub dim: TexDim,
    pub offset_mode: TexOffsetMode,
    pub mem_eviction_priority: MemEvictionPriority,
    pub nodep: bool,
    pub channel_mask: ChannelMask,
}

impl DisplayOp for OpTxd {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "txd{}{}{}",
            self.dim, self.offset_mode, self.mem_eviction_priority
        )?;
        if self.nodep {
            write!(f, ".nodep")?;
        }
        write!(f, "{}", self.channel_mask)?;
        write!(f, " {} {} {}", self.tex, self.srcs[0], self.srcs[1])
    }
}
impl_display_for_op!(OpTxd);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpTxq {
    pub dsts: [Dst; 2],

    pub tex: TexRef,

    #[src_type(SSA)]
    pub src: Src,

    pub query: TexQuery,
    pub nodep: bool,
    pub channel_mask: ChannelMask,
}

impl DisplayOp for OpTxq {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "txq")?;
        if self.nodep {
            write!(f, ".nodep")?;
        }
        write!(f, "{}", self.channel_mask)?;
        write!(f, " {} {} {}", self.tex, self.src, self.query)
    }
}
impl_display_for_op!(OpTxq);

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, PartialEq)]
pub enum ImageAccess {
    Binary(MemType),
    Formatted(ChannelMask),
}

impl fmt::Display for ImageAccess {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ImageAccess::Binary(mem_type) => write!(f, ".b{mem_type}"),
            ImageAccess::Formatted(mask) => write!(f, ".p{mask}"),
        }
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSuLd {
    pub dst: Dst,
    pub fault: Dst,

    pub image_access: ImageAccess,
    pub image_dim: ImageDim,
    pub mem_order: MemOrder,
    pub mem_eviction_priority: MemEvictionPriority,

    #[src_type(SSA)]
    pub handle: Src,

    #[src_type(SSA)]
    pub coord: Src,
}

impl DisplayOp for OpSuLd {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "suld{}{}{}{} [{}] {}",
            self.image_access,
            self.image_dim,
            self.mem_order,
            self.mem_eviction_priority,
            self.coord,
            self.handle,
        )
    }
}
impl_display_for_op!(OpSuLd);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSuSt {
    pub image_access: ImageAccess,
    pub image_dim: ImageDim,
    pub mem_order: MemOrder,
    pub mem_eviction_priority: MemEvictionPriority,

    #[src_type(SSA)]
    pub handle: Src,

    #[src_type(SSA)]
    pub coord: Src,

    #[src_type(SSA)]
    pub data: Src,
}

impl DisplayOp for OpSuSt {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "sust{}{}{}{} [{}] {} {}",
            self.image_access,
            self.image_dim,
            self.mem_order,
            self.mem_eviction_priority,
            self.coord,
            self.data,
            self.handle,
        )
    }
}
impl_display_for_op!(OpSuSt);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSuAtom {
    pub dst: Dst,
    pub fault: Dst,

    pub image_dim: ImageDim,

    pub atom_op: AtomOp,
    pub atom_type: AtomType,

    pub mem_order: MemOrder,
    pub mem_eviction_priority: MemEvictionPriority,

    #[src_type(SSA)]
    pub handle: Src,

    #[src_type(SSA)]
    pub coord: Src,

    #[src_type(SSA)]
    pub data: Src,
}

impl DisplayOp for OpSuAtom {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "suatom.p{}{}{}{}{} [{}] {} {}",
            self.image_dim,
            self.atom_op,
            self.atom_type,
            self.mem_order,
            self.mem_eviction_priority,
            self.coord,
            self.data,
            self.handle,
        )
    }
}
impl_display_for_op!(OpSuAtom);

#[derive(Clone, Copy)]
pub enum SuClampMode {
    StoredInDescriptor,
    PitchLinear,
    BlockLinear,
}

impl fmt::Display for SuClampMode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let s = match self {
            SuClampMode::StoredInDescriptor => ".sd",
            SuClampMode::PitchLinear => ".pl",
            SuClampMode::BlockLinear => ".bl",
        };
        write!(f, "{}", s)
    }
}

#[derive(Clone, Copy)]
pub enum SuClampRound {
    R1,
    R2,
    R4,
    R8,
    R16,
}

impl SuClampRound {
    pub fn to_int(&self) -> u8 {
        match self {
            SuClampRound::R1 => 1,
            SuClampRound::R2 => 2,
            SuClampRound::R4 => 4,
            SuClampRound::R8 => 8,
            SuClampRound::R16 => 16,
        }
    }

    pub fn to_mask(&self) -> u32 {
        !(self.to_int() as u32 - 1)
    }
}

impl fmt::Display for SuClampRound {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, ".r{}", self.to_int())
    }
}

/// Kepler only
/// Surface Clamp
///
/// Can clamp coordinates of surface operations in a 0..=clamp inclusive
/// range. It also computes other information useful to compute the
/// real address of an element within an image for both block-lienar and
/// pitch-linear layouts. We can also reduce this operation to a "stupid"
/// inclusive clamp by setting modifier Mode=PitchLinear and is_2d=false
/// this will not compute any extra operations and is useful to clamp array
/// indexes.
///
/// Since the shader code does not know if an image layout is block-linear
/// or pitch-linear, this opcode must be able to do both, the operation
/// is then selected by the "clamp" bitfield, usually read from a descriptor.
/// In block-linear mode we divide the bits that will compute the higher
/// part and the lower part.
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice, Clone)]
pub struct OpSuClamp {
    #[dst_type(GPR)]
    pub dst: Dst,
    #[dst_type(Pred)]
    pub out_of_bounds: Dst,

    /// This modifier specifies if we use pitch-linear or block-linear
    /// calculations, another option is to support both and read the actual
    /// format from the clamp (shader code doesn't always know if an image
    /// layout).
    /// When mode=pitch_linear and is_2d=false the suclamp op enters a
    /// simpler "plain" mode where it only performs clamping and the output
    /// register doesn't contain any information bits about pitch-linear or
    /// block-linear calculations
    pub mode: SuClampMode,
    /// Strangely enough, "round" just rounds the clamp, not the source
    /// this does not help at all with clamping coordinates.
    /// It could be useful when clamping raw addresses of a multi-byte read.
    /// ex: if we read 4 bytes at once, and the buffer length is 16,
    ///     the bounds will be 15 (they are inclusive), but if we read
    ///     at address 15 we would read bytes 15..19, so we are out of range.
    ///     if we clamp tthe bounds to R4 the effective bound becomes 12
    ///     so the read will be performed from 12..16, remaining in bounds.
    pub round: SuClampRound,
    pub is_s32: bool,
    pub is_2d: bool,

    #[src_type(GPR)]
    pub coords: Src,

    /// Packed parameter containing both bounds (inclusive)
    /// and other information (explained in more details in Foldable):
    /// 0..20: bound (inclusive)
    /// 21: pitch_linear (used if mode == StoredInDescriptor)
    /// 22..26: coord shl
    /// 26..29: coord shr
    /// 29..32: n. of tiles
    #[src_type(ALU)]
    pub params: Src,
    /// Added to the coords, it's only an i6
    pub imm: i8,
}

impl Foldable for OpSuClamp {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let src = f.get_u32_src(self, &self.coords);
        let params = f.get_u32_src(self, &self.params);
        let imm = self.imm; // i6

        let src = if self.is_s32 {
            (src as i32) as i64
        } else {
            src as i64
        };
        let src = src + (imm as i64);

        let params_bv = BitView::new(&params);
        let pitch_linear = match self.mode {
            SuClampMode::StoredInDescriptor => params_bv.get_bit(21),
            SuClampMode::PitchLinear => true,
            SuClampMode::BlockLinear => false,
        };

        let bounds = if pitch_linear && !self.is_2d {
            params
        } else {
            params_bv.get_bit_range_u64(0..20) as u32
        };

        let bounds = bounds & self.round.to_mask();
        let (is_oob, clamped) = if src < 0 {
            (true, 0)
        } else if src > (bounds as i64) {
            (true, bounds)
        } else {
            (false, src as u32)
        };

        let mut out = 0u32;
        let mut bv = BitMutView::new(&mut out);
        if pitch_linear {
            if !self.is_2d {
                // simple clamp mode, NO BITFIELD
                bv.set_field(0..32, clamped);
            } else {
                // Real, pitch_linear mode
                bv.set_field(0..20, clamped & 0xfffff);

                // Pass through el_size_log2
                bv.set_field(27..30, params_bv.get_bit_range_u64(26..29));
                bv.set_bit(30, true); // pitch_linear=true
                bv.set_bit(31, is_oob);
            }
        } else {
            // Block linear

            // Number of bits to discard for GoB coordinates
            let shr_a = params_bv.get_bit_range_u64(22..26) as u8;
            // Block coords
            bv.set_field(0..16, (clamped >> shr_a) & 0xffff);

            // Shift applied to coords, always zero except for x.
            // (for coord x=1 and format R32, we want to access byte 4)
            // e.g. R8 -> 0, R32 -> 2, 128 -> 4
            let el_size_log2 = params_bv.get_bit_range_u64(26..29) as u8;
            // Coord inside GoB (element space)
            bv.set_field(16..24, (clamped << el_size_log2) & 0xff);

            // Useful later to compute gob-space coords.
            let n_tiles = params_bv.get_bit_range_u64(29..32) as u8;
            bv.set_field(27..30, n_tiles);
            bv.set_bit(30, false); // pitch_linear=false
            bv.set_bit(31, is_oob);
        }
        f.set_u32_dst(self, &self.dst, out);
        f.set_pred_dst(self, &self.out_of_bounds, is_oob);
    }
}

impl DisplayOp for OpSuClamp {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "suclamp{}", self.mode)?;
        if !matches!(self.round, SuClampRound::R1) {
            write!(f, "{}", self.round)?;
        }
        if !self.is_s32 {
            write!(f, ".u32")?;
        }
        if !self.is_2d {
            write!(f, ".1d")?;
        }

        write! {f, " {} {} {:x}", self.coords, self.params, self.imm}
    }
}
impl_display_for_op!(OpSuClamp);

/// Kepler only
/// BitField Merge
///
/// The resulting bit-field is composed of a high-part 8..32 that is merged
/// with the address by sueau, and a lower-part 0..8 that is provided
/// directly to suldga/sustga and defines the lower offset of the glonal array.
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice, Clone)]
pub struct OpSuBfm {
    #[dst_type(GPR)]
    pub dst: Dst,
    #[dst_type(Pred)]
    pub pdst: Dst,

    /// x, y, z
    #[src_type(ALU)]
    pub srcs: [Src; 3],
    /// When is_3d=false the third source is ignored, but still used in
    /// pitch-linear computation.
    pub is_3d: bool,
}

impl Foldable for OpSuBfm {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let x_raw = f.get_u32_src(self, &self.srcs[0]);
        let y_raw = f.get_u32_src(self, &self.srcs[1]);
        let z_raw = f.get_u32_src(self, &self.srcs[2]);

        let x = BitView::new(&x_raw);
        let y = BitView::new(&y_raw);
        let z = BitView::new(&z_raw);

        let mut o_raw = 0u32;
        let mut o = BitMutView::new(&mut o_raw);

        let is_pitch_linear_2d = x.get_bit(30) || y.get_bit(30);

        if !is_pitch_linear_2d {
            // Copy coordinates inside of GoB space.
            // They are 6 bits from x and 3 from y (GoB is 64x8 bytes).
            // Bits from 0..8 are ignored by sueau and are used directly
            // by suldga/sustga.
            // Bit 9 will become the first bit of the higher part in
            // sueau.
            o.set_bit_range_u64(0..4, x.get_bit_range_u64(16..20));

            // Address calculation inside of GoB should virtually be
            // y * 64 + x * element_size (each row is linear).
            // So why are those bits swizzled like so?
            // I have no idea, but these are correct even for atomics
            // that accept real addresses.
            o.set_bit(4, y.get_bit(16));
            o.set_bit(5, y.get_bit(17));
            o.set_bit(6, x.get_bit(20));
            o.set_bit(7, y.get_bit(18));

            o.set_bit(8, x.get_bit(21));
            // 9..11: 0

            // -------------- Tiles --------------
            // Number of tiles log2
            let ntx = x.get_bit_range_u64(27..30) & 0x1;
            let nty = y.get_bit_range_u64(27..30);
            let ntz = z.get_bit_range_u64(27..30);
            let ntz = ntz * (self.is_3d as u64); // z is ignored if is_3d=false

            // Computes how many bits to dedicate to GoB coords inside
            // a block
            o.set_field(12..16, ntx + nty + ntz);

            // Coords in gob_space.
            // Remove 6 bits from x and 3 bits from y, those are used
            // as element coords in GoB space.
            let a = x.get_bit_range_u64(22..24); // 1100_0000
            let b = y.get_bit_range_u64(19..24); // 1111_1000
            let c = z.get_bit_range_u64(16..24); // 1111_1111

            // nt* indicates how many bits to consider (max 5)
            let a = a & ((1 << ntx) - 1);
            let b = b & ((1 << nty.min(5)) - 1);
            let c = c & ((1 << ntz.min(5)) - 1);

            // Compute gob offset
            // We can just or together at certain offsets because
            // Tiles are always powers of two in each direction.
            // z || y || x (LSB)
            let res = c;
            let res = (res << nty) | b;
            let res = (res << ntx) | a;
            let mask = match ntx {
                0 => 0x3ff,
                _ => 0x7ff,
            };

            // gob coords will be put before the block coords in
            // sueau.
            o.set_field(16..27, res & mask);
        } else {
            let d = z.get_bit_range_u64(0..8);
            let el_size_log2 = x.get_bit_range_u64(27..30);
            o.set_field(0..8, (d << el_size_log2) & 0xff);
            // 9..11: 0
            o.set_field(12..15, el_size_log2);
        }

        o.set_bit(11, is_pitch_linear_2d);

        let is_oob =
            x.get_bit(31) || y.get_bit(31) || (z.get_bit(31) && self.is_3d);
        f.set_u32_dst(self, &self.dst, o_raw);
        f.set_pred_dst(self, &self.pdst, is_oob);
    }
}

impl DisplayOp for OpSuBfm {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "subfm")?;

        if self.is_3d {
            write!(f, ".3d")?;
        }

        write!(f, " {} {} {}", self.srcs[0], self.srcs[1], self.srcs[2])
    }
}
impl_display_for_op!(OpSuBfm);

/// Kepler only
/// Used to compute the higher 32 bits of image address using
/// the merged bitfield and the block coordinates (offset).
/// It can switch to a pitch_linear mode (bit 11 of bit-field).
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice, Clone)]
pub struct OpSuEau {
    #[dst_type(GPR)]
    pub dst: Dst,

    /// offset is computed from the block coordinates.
    /// it's ok to add it directly to the address since they are both
    /// "aligned" to 64 (the first 8 bits are removed from both)
    #[src_type(GPR)]
    pub off: Src,

    ///  8.. 9: offset, last bit
    /// 11..12: pitch_linear: when enabled the bf-offset is ignored and
    ///         the off_shl is subtracted by 8
    /// 12..16: off_shl, shifts left the offset by off_shl + 1
    /// 16..27: 11-bit offset, when joined with the 1-bit offset completes the
    ///         12-bit offset ORed to the src offset after shifting
    ///         (unless pitch_linear)
    #[src_type(ALU)]
    pub bit_field: Src,

    #[src_type(GPR)]
    pub addr: Src,
}

impl Foldable for OpSuEau {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let off_raw = f.get_u32_src(self, &self.off);
        let bf_raw = f.get_u32_src(self, &self.bit_field);
        let addr = f.get_u32_src(self, &self.addr);

        let bf = BitView::new(&bf_raw);

        let off1 = bf.get_bit_range_u64(8..9) as u32;
        let is_pitch_linear = bf.get_bit(11);
        let off_shift = bf.get_bit_range_u64(12..16) as u32;
        let offs = bf.get_bit_range_u64(16..27) as u32;

        let res = if !is_pitch_linear {
            // Block linear
            // off_raw are the block coordinates
            // to those we add gob coordinates from the merged bitfield
            // and the MSB of in-gob coordinates.
            let omul = off_shift + 1;
            let real_off = (off_raw << omul) | (offs << 1) | off1;
            addr.wrapping_add(real_off & 0x7ff_ffff)
        } else {
            // Add the high part of the coordinates to addr
            // off << (omul - 8)
            // but for negative values do a shr instead.
            // In fact, off_shift will always be < 8 because pitch_linear
            // subfm only assigns bits 12..15, so this is always a shr
            let shl_amount = off_shift as i32 - 8;
            let off = if shl_amount < 0 {
                off_raw >> (-shl_amount as u32)
            } else {
                off_raw << (shl_amount as u32)
            };
            addr.wrapping_add(off & 0xff_ffff)
        };
        f.set_u32_dst(self, &self.dst, res);
    }
}

impl DisplayOp for OpSuEau {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write! {f, "sueau {} {} {}", self.off, self.bit_field, self.addr}
    }
}
impl_display_for_op!(OpSuEau);

#[derive(Copy, Clone, Debug)]
pub enum IMadSpSrcType {
    U32,
    U24,
    U16Hi,
    U16Lo,
    S32,
    S24,
    S16Hi,
    S16Lo,
}

impl IMadSpSrcType {
    pub fn unsigned(self) -> IMadSpSrcType {
        use IMadSpSrcType::*;
        match self {
            S32 => U32,
            S24 => U24,
            S16Hi => U16Hi,
            S16Lo => U16Lo,
            x => x,
        }
    }

    #[allow(dead_code)] // Used in hw_tests
    pub fn with_sign(self, sign: bool) -> Self {
        use IMadSpSrcType::*;
        if !sign {
            return self.unsigned();
        }
        match self {
            U32 => S32,
            U24 => S24,
            U16Hi => S16Hi,
            U16Lo => S16Lo,
            x => x,
        }
    }

    pub fn sign(self) -> bool {
        use IMadSpSrcType::*;
        match self {
            U32 | U24 | U16Hi | U16Lo => false,
            S32 | S24 | S16Hi | S16Lo => true,
        }
    }

    fn cast(&self, v: u32) -> i64 {
        use IMadSpSrcType::*;
        match self {
            U32 => v as i64,
            U24 => (v & 0x00ff_ffff) as i64,
            U16Lo => (v as u16) as i64,
            U16Hi => (v >> 16) as i64,
            S32 => (v as i32) as i64,
            S24 => (((v as i32) << 8) >> 8) as i64, // Sign extend
            S16Lo => (v as i16) as i64,
            S16Hi => ((v >> 16) as i16) as i64,
        }
    }
}

impl fmt::Display for IMadSpSrcType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let sign = if self.sign() { ".s" } else { ".u" };
        let width = match self.unsigned() {
            IMadSpSrcType::U32 => "32",
            IMadSpSrcType::U24 => "24",
            IMadSpSrcType::U16Lo => "16h0",
            IMadSpSrcType::U16Hi => "16h1",
            _ => unreachable!(),
        };
        write!(f, "{}{}", sign, width)
    }
}

#[derive(Clone, Copy, Debug)]
pub enum IMadSpMode {
    Explicit([IMadSpSrcType; 3]),
    // Parameters are loaded from src1 bits 26..32
    FromSrc1,
}

impl fmt::Display for IMadSpMode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            IMadSpMode::Explicit([a, b, c]) => write!(f, "{a}{b}{c}"),
            IMadSpMode::FromSrc1 => write!(f, ".sd"),
        }
    }
}

/// Kepler only
/// Extracted Integer Multiply and Add.
/// It does the same operation as an imad op, but it can extract the
/// sources from a subset of the register (only 32, 24 or 16 bits).
/// It can also do a "load parameters" mode where the modifiers are
/// loaded from the higher bits in src2 (check Foldable impl for details).
/// Limits: src1 can never be U32 or U16Hi,
///         src2 can never be U16Hi
///         src2 signedness is tied to src1 and src0 signedness,
///           if either is signed, src2 must be signed too.
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice, Clone)]
pub struct OpIMadSp {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(ALU)]
    pub srcs: [Src; 3],

    pub mode: IMadSpMode,
}

impl Foldable for OpIMadSp {
    fn fold(&self, _sm: &dyn ShaderModel, f: &mut OpFoldData<'_>) {
        let src0 = f.get_u32_src(self, &self.srcs[0]);
        let src1 = f.get_u32_src(self, &self.srcs[1]);
        let src2 = f.get_u32_src(self, &self.srcs[2]);

        let (src_type0, src_type1, src_type2) = match self.mode {
            IMadSpMode::Explicit([t0, t1, t2]) => (t0, t1, t2),
            IMadSpMode::FromSrc1 => {
                let params = BitView::new(&src1);

                let st2 = params.get_bit_range_u64(26..28) as usize;
                let st1 = params.get_bit_range_u64(28..30) as usize;
                let st0 = params.get_bit_range_u64(30..32) as usize;

                use IMadSpSrcType::*;
                let types0 = [U32, U24, U16Lo, U16Hi];
                let types1 = [U16Lo, U24, U16Lo, U24];
                let types2 = [U32, U24, U16Lo, U32];

                (
                    types0[st0].unsigned(),
                    types1[st1].unsigned(),
                    types2[st2].unsigned(),
                )
            }
        };

        let src0 = src_type0.cast(src0);
        let src1 = src_type1.cast(src1);
        let src2 = src_type2.cast(src2);

        f.set_u32_dst(self, &self.dst, (src0 * src1 + src2) as u32);
    }
}

impl DisplayOp for OpIMadSp {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "imadsp{} {} {} {}",
            self.mode, self.srcs[0], self.srcs[1], self.srcs[2]
        )
    }
}
impl_display_for_op!(OpIMadSp);

/// In SuGa ops, the address is always specified in two parts, the higher
/// part contains the base address without the lower 8 bits (base_addr >> 8),
/// while the lower part might contain either the missing 8 bits (U8) or
/// a full 32-bit offset that must not be shifted (U32).
///
/// In short:
/// U8 : real_address = (addr_hi << 8) + (addr_lo & 0xFF)
/// U32: real_address = (addr_hi << 8) + addr_lo
/// The signed variants do the same but with sign extension probably
#[derive(Clone, Copy)]
pub enum SuGaOffsetMode {
    U32,
    S32,
    U8,
    S8,
}

/// Kepler only
/// Load a pixel from an image, takes the pixel address and format as an
/// argument. Since the image coordinates are not present, the instruction
/// also needs an `out_of_bounds` predicate, when true it always load (0, 0, 0, 1)
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSuLdGa {
    pub dst: Dst,

    pub mem_type: MemType,
    pub offset_mode: SuGaOffsetMode,
    pub cache_op: LdCacheOp,

    /// Format for the loaded data, passed directly from the descriptor.
    #[src_type(GPR)]
    pub format: Src,

    /// This is not an address, but it's two registers that contain
    /// [addr >> 8, addr & 0xff].
    /// This works because addr >> 8 is 32-bits (GOB-aligned) and the
    /// rest 8-bits are extracted by the bit-field
    /// It's useful since in block-linear mode the lower bits and the higher
    /// bits are computed in different ways.
    #[src_type(SSA)]
    pub addr: Src,

    #[src_type(Pred)]
    pub out_of_bounds: Src,
}

impl DisplayOp for OpSuLdGa {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "suldga{}{} [{}] {} {}",
            self.mem_type,
            self.cache_op,
            self.addr,
            self.format,
            self.out_of_bounds
        )
    }
}
impl_display_for_op!(OpSuLdGa);

/// Kepler only
/// Store a pixel in an image, takes the pixel address and format as an
/// argument. Since the image coordinates are not present, the instruction
/// also needs an `out_of_bounds` predicate, when true, stores are ingored
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSuStGa {
    pub image_access: ImageAccess,
    pub offset_mode: SuGaOffsetMode,
    pub cache_op: StCacheOp,

    #[src_type(GPR)]
    pub format: Src,

    #[src_type(SSA)]
    pub addr: Src,

    #[src_type(SSA)]
    pub data: Src,

    #[src_type(Pred)]
    pub out_of_bounds: Src,
}

impl DisplayOp for OpSuStGa {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "sustga{}{} [{}] {} {} {}",
            self.image_access,
            self.cache_op,
            self.addr,
            self.format,
            self.data,
            self.out_of_bounds,
        )
    }
}
impl_display_for_op!(OpSuStGa);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpLd {
    pub dst: Dst,

    #[src_type(GPR)]
    pub addr: Src,

    pub offset: i32,
    pub access: MemAccess,
}

impl DisplayOp for OpLd {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "ld{} [{}", self.access, self.addr)?;
        if self.offset > 0 {
            write!(f, "+{:#x}", self.offset)?;
        }
        write!(f, "]")
    }
}
impl_display_for_op!(OpLd);

#[allow(dead_code)]
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum LdcMode {
    Indexed,
    IndexedLinear,
    IndexedSegmented,
    IndexedSegmentedLinear,
}

impl fmt::Display for LdcMode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LdcMode::Indexed => Ok(()),
            LdcMode::IndexedLinear => write!(f, ".il"),
            LdcMode::IndexedSegmented => write!(f, ".is"),
            LdcMode::IndexedSegmentedLinear => write!(f, ".isl"),
        }
    }
}

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpLdc {
    pub dst: Dst,

    #[src_type(ALU)]
    pub cb: Src,

    #[src_type(GPR)]
    pub offset: Src,

    pub mode: LdcMode,
    pub mem_type: MemType,
}

impl DisplayOp for OpLdc {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let SrcRef::CBuf(cb) = &self.cb.src_ref else {
            panic!("Not a cbuf");
        };
        write!(f, "ldc{}{} {}[", self.mode, self.mem_type, cb.buf)?;
        if self.offset.is_zero() {
            write!(f, "+{:#x}", cb.offset)?;
        } else if cb.offset == 0 {
            write!(f, "{}", self.offset)?;
        } else {
            write!(f, "{}+{:#x}", self.offset, cb.offset)?;
        }
        write!(f, "]")
    }
}
impl_display_for_op!(OpLdc);

/// Used for Kepler to implement shared atomics.
/// In addition to the load, it tries to lock the address,
/// Kepler hardware has (1024?) hardware mutex locks.
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpLdSharedLock {
    pub dst: Dst,
    #[dst_type(Pred)]
    pub locked: Dst,

    #[src_type(GPR)]
    pub addr: Src,

    pub offset: i32,
    pub mem_type: MemType,
}

impl DisplayOp for OpLdSharedLock {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "ldslk{} [{}", self.mem_type, self.addr)?;
        if self.offset > 0 {
            write!(f, "+{:#x}", self.offset)?;
        }
        write!(f, "]")
    }
}
impl_display_for_op!(OpLdSharedLock);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSt {
    #[src_type(GPR)]
    pub addr: Src,

    #[src_type(SSA)]
    pub data: Src,

    pub offset: i32,
    pub access: MemAccess,
}

impl DisplayOp for OpSt {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "st{} [{}", self.access, self.addr)?;
        if self.offset > 0 {
            write!(f, "+{:#x}", self.offset)?;
        }
        write!(f, "] {}", self.data)
    }
}
impl_display_for_op!(OpSt);

/// Used for Kepler to implement shared atomics.
/// It checks that the address is still properly locked, performs the
/// store operation and unlocks the previously unlocked address.
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpStSCheckUnlock {
    #[dst_type(Pred)]
    pub locked: Dst,

    #[src_type(GPR)]
    pub addr: Src,
    #[src_type(SSA)]
    pub data: Src,

    pub offset: i32,
    pub mem_type: MemType,
}

impl DisplayOp for OpStSCheckUnlock {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "stscul{} [{}", self.mem_type, self.addr)?;
        if self.offset > 0 {
            write!(f, "+{:#x}", self.offset)?;
        }
        write!(f, "] {}", self.data)
    }
}
impl_display_for_op!(OpStSCheckUnlock);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpAtom {
    pub dst: Dst,

    #[src_type(GPR)]
    pub addr: Src,

    #[src_type(GPR)]
    pub cmpr: Src,

    #[src_type(SSA)]
    pub data: Src,

    pub atom_op: AtomOp,
    pub atom_type: AtomType,

    pub addr_offset: i32,

    pub mem_space: MemSpace,
    pub mem_order: MemOrder,
    pub mem_eviction_priority: MemEvictionPriority,
}

impl DisplayOp for OpAtom {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "atom{}{}{}{}{}",
            self.atom_op,
            self.atom_type,
            self.mem_space,
            self.mem_order,
            self.mem_eviction_priority,
        )?;
        write!(f, " [")?;
        if !self.addr.is_zero() {
            write!(f, "{}", self.addr)?;
        }
        if self.addr_offset > 0 {
            if !self.addr.is_zero() {
                write!(f, "+")?;
            }
            write!(f, "{:#x}", self.addr_offset)?;
        }
        write!(f, "]")?;
        if self.atom_op == AtomOp::CmpExch(AtomCmpSrc::Separate) {
            write!(f, " {}", self.cmpr)?;
        }
        write!(f, " {}", self.data)
    }
}
impl_display_for_op!(OpAtom);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpAL2P {
    pub dst: Dst,

    #[src_type(GPR)]
    pub offset: Src,

    pub addr: u16,
    pub comps: u8,
    pub output: bool,
}

impl DisplayOp for OpAL2P {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "al2p")?;
        if self.output {
            write!(f, ".o")?;
        }
        write!(f, " a[{:#x}", self.addr)?;
        if !self.offset.is_zero() {
            write!(f, "+{}", self.offset)?;
        }
        write!(f, "]")
    }
}
impl_display_for_op!(OpAL2P);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpALd {
    pub dst: Dst,

    #[src_type(GPR)]
    pub vtx: Src,

    #[src_type(GPR)]
    pub offset: Src,

    pub addr: u16,
    pub comps: u8,
    pub patch: bool,
    pub output: bool,
    pub phys: bool,
}

impl DisplayOp for OpALd {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "ald")?;
        if self.output {
            write!(f, ".o")?;
        }
        if self.patch {
            write!(f, ".p")?;
        }
        if self.phys {
            write!(f, ".phys")?;
        }
        write!(f, " a")?;
        if !self.vtx.is_zero() {
            write!(f, "[{}]", self.vtx)?;
        }
        write!(f, "[{:#x}", self.addr)?;
        if !self.offset.is_zero() {
            write!(f, "+{}", self.offset)?;
        }
        write!(f, "]")
    }
}
impl_display_for_op!(OpALd);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpASt {
    #[src_type(GPR)]
    pub vtx: Src,

    #[src_type(GPR)]
    pub offset: Src,

    #[src_type(SSA)]
    pub data: Src,

    pub addr: u16,
    pub comps: u8,
    pub patch: bool,
    pub phys: bool,
}

impl DisplayOp for OpASt {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "ast")?;
        if self.patch {
            write!(f, ".p")?;
        }
        if self.phys {
            write!(f, ".phys")?;
        }
        write!(f, " a")?;
        if !self.vtx.is_zero() {
            write!(f, "[{}]", self.vtx)?;
        }
        write!(f, "[{:#x}", self.addr)?;
        if !self.offset.is_zero() {
            write!(f, "+{}", self.offset)?;
        }
        write!(f, "] {}", self.data)
    }
}
impl_display_for_op!(OpASt);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpIpa {
    pub dst: Dst,
    pub addr: u16,
    pub freq: InterpFreq,
    pub loc: InterpLoc,
    pub inv_w: Src,
    pub offset: Src,
}

impl DisplayOp for OpIpa {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "ipa{}{} a[{:#x}] {}",
            self.freq, self.loc, self.addr, self.inv_w
        )?;
        if self.loc == InterpLoc::Offset {
            write!(f, " {}", self.offset)?;
        }
        Ok(())
    }
}
impl_display_for_op!(OpIpa);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpLdTram {
    pub dst: Dst,
    pub addr: u16,
    pub use_c: bool,
}

impl DisplayOp for OpLdTram {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "ldtram")?;
        if self.use_c {
            write!(f, ".c")?;
        } else {
            write!(f, ".ab")?;
        }
        write!(f, " a[{:#x}]", self.addr)?;
        Ok(())
    }
}
impl_display_for_op!(OpLdTram);

#[allow(dead_code)]
#[derive(Copy, Clone, Debug)]
pub enum CCtlOp {
    Qry1, // Only available pre-Volta
    PF1,
    PF1_5, // Only available pre-Volta
    PF2,
    WB,
    IV,
    IVAll,
    RS,
    RSLB,   // Only available pre-Volta
    IVAllP, // Only available on Volta+
    WBAll,  // Only available on Volta+
    WBAllP, // Only available on Volta+
}

impl CCtlOp {
    pub fn is_all(&self) -> bool {
        match self {
            CCtlOp::Qry1
            | CCtlOp::PF1
            | CCtlOp::PF1_5
            | CCtlOp::PF2
            | CCtlOp::WB
            | CCtlOp::IV
            | CCtlOp::RS
            | CCtlOp::RSLB => false,
            CCtlOp::IVAll | CCtlOp::IVAllP | CCtlOp::WBAll | CCtlOp::WBAllP => {
                true
            }
        }
    }
}

impl fmt::Display for CCtlOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CCtlOp::Qry1 => write!(f, "qry1"),
            CCtlOp::PF1 => write!(f, "pf1"),
            CCtlOp::PF1_5 => write!(f, "pf1.5"),
            CCtlOp::PF2 => write!(f, "pf2"),
            CCtlOp::WB => write!(f, "wb"),
            CCtlOp::IV => write!(f, "iv"),
            CCtlOp::IVAll => write!(f, "ivall"),
            CCtlOp::RS => write!(f, "rs"),
            CCtlOp::RSLB => write!(f, "rslb"),
            CCtlOp::IVAllP => write!(f, "ivallp"),
            CCtlOp::WBAll => write!(f, "wball"),
            CCtlOp::WBAllP => write!(f, "wballp"),
        }
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpCCtl {
    pub op: CCtlOp,

    pub mem_space: MemSpace,

    #[src_type(GPR)]
    pub addr: Src,

    pub addr_offset: i32,
}

impl DisplayOp for OpCCtl {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "cctl{}", self.mem_space)?;
        if !self.op.is_all() {
            write!(f, " [{}", self.addr)?;
            if self.addr_offset > 0 {
                write!(f, "+{:#x}", self.addr_offset)?;
            }
            write!(f, "]")?;
        }
        Ok(())
    }
}
impl_display_for_op!(OpCCtl);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpMemBar {
    pub scope: MemScope,
}

impl DisplayOp for OpMemBar {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "membar.sc.{}", self.scope)
    }
}
impl_display_for_op!(OpMemBar);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpBClear {
    pub dst: Dst,
}

impl DisplayOp for OpBClear {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "bclear")
    }
}
impl_display_for_op!(OpBClear);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpBMov {
    pub dst: Dst,
    pub src: Src,
    pub clear: bool,
}

impl DisplayOp for OpBMov {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "bmov.32")?;
        if self.clear {
            write!(f, ".clear")?;
        }
        write!(f, " {}", self.src)
    }
}
impl_display_for_op!(OpBMov);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpBreak {
    #[dst_type(Bar)]
    pub bar_out: Dst,

    #[src_type(Bar)]
    pub bar_in: Src,

    #[src_type(Pred)]
    pub cond: Src,
}

impl DisplayOp for OpBreak {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "break {} {}", self.bar_in, self.cond)
    }
}
impl_display_for_op!(OpBreak);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpBSSy {
    #[dst_type(Bar)]
    pub bar_out: Dst,

    #[src_type(Pred)]
    pub bar_in: Src,

    #[src_type(Pred)]
    pub cond: Src,

    pub target: Label,
}

impl DisplayOp for OpBSSy {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "bssy {} {} {}", self.bar_in, self.cond, self.target)
    }
}
impl_display_for_op!(OpBSSy);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpBSync {
    #[src_type(Bar)]
    pub bar: Src,

    #[src_type(Pred)]
    pub cond: Src,
}

impl DisplayOp for OpBSync {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "bsync {} {}", self.bar, self.cond)
    }
}
impl_display_for_op!(OpBSync);

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpBra {
    pub target: Label,
}

impl DisplayOp for OpBra {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "bra {}", self.target)
    }
}
impl_display_for_op!(OpBra);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSSy {
    pub target: Label,
}

impl DisplayOp for OpSSy {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "ssy {}", self.target)
    }
}
impl_display_for_op!(OpSSy);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSync {
    pub target: Label,
}

impl DisplayOp for OpSync {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "sync {}", self.target)
    }
}
impl_display_for_op!(OpSync);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpBrk {
    pub target: Label,
}

impl DisplayOp for OpBrk {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "brk {}", self.target)
    }
}
impl_display_for_op!(OpBrk);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpPBk {
    pub target: Label,
}

impl DisplayOp for OpPBk {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "pbk {}", self.target)
    }
}
impl_display_for_op!(OpPBk);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpCont {
    pub target: Label,
}

impl DisplayOp for OpCont {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "cont {}", self.target)
    }
}
impl_display_for_op!(OpCont);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpPCnt {
    pub target: Label,
}

impl DisplayOp for OpPCnt {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "pcnt {}", self.target)
    }
}
impl_display_for_op!(OpPCnt);

#[repr(C)]
#[derive(Clone, SrcsAsSlice, DstsAsSlice)]
pub struct OpExit {}

impl DisplayOp for OpExit {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "exit")
    }
}
impl_display_for_op!(OpExit);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpWarpSync {
    pub mask: u32,
}

impl DisplayOp for OpWarpSync {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "warpsync 0x{:x}", self.mask)
    }
}
impl_display_for_op!(OpWarpSync);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpBar {}

impl DisplayOp for OpBar {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "bar.sync")
    }
}
impl_display_for_op!(OpBar);

/// Instruction only used on Kepler(A|B).
/// Kepler has explicit dependency tracking for texture loads.
/// When a texture load is executed, it is put on some kind of FIFO queue
/// for later execution.
/// Before the results of a texture are used we need to wait on the queue,
/// texdepbar waits until the queue has at most `textures_left` elements.
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpTexDepBar {
    pub textures_left: u8,
}

impl OpTexDepBar {
    /// Maximum value of textures_left
    ///
    /// The maximum encodable value is 63.  However, nvcc starts emitting
    /// TEXDEPBAR 0x3e as soon as it hits 62 texture instructions.
    pub const MAX_TEXTURES_LEFT: u8 = 62;
}

impl DisplayOp for OpTexDepBar {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "texdepbar {}", self.textures_left)
    }
}
impl_display_for_op!(OpTexDepBar);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpCS2R {
    pub dst: Dst,
    pub idx: u8,
}

impl DisplayOp for OpCS2R {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "cs2r sr[{:#x}]", self.idx)
    }
}
impl_display_for_op!(OpCS2R);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpIsberd {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(SSA)]
    pub idx: Src,
}

impl DisplayOp for OpIsberd {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "isberd [{}]", self.idx)
    }
}
impl_display_for_op!(OpIsberd);

/// Vertex Index Load
/// (Only available in Kepler)
///
/// Takes as input the vertex index and loads the vertex address in
/// attribute space.
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpViLd {
    #[dst_type(GPR)]
    pub dst: Dst,

    #[src_type(SSA)]
    pub idx: Src,

    pub off: i8,
}

impl DisplayOp for OpViLd {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "vild v[")?;

        if !self.idx.is_zero() {
            write!(f, "{}", self.idx)?;
            if self.off != 0 {
                write!(f, "{:+}", self.off)?;
            }
        } else {
            write!(f, "{}", self.off)?;
        }

        write!(f, "]")
    }
}
impl_display_for_op!(OpViLd);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpKill {}

impl DisplayOp for OpKill {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "kill")
    }
}
impl_display_for_op!(OpKill);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpNop {
    pub label: Option<Label>,
}

impl DisplayOp for OpNop {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "nop")?;
        if let Some(label) = &self.label {
            write!(f, " {}", label)?;
        }
        Ok(())
    }
}
impl_display_for_op!(OpNop);

#[allow(dead_code)]
pub enum PixVal {
    MsCount,
    CovMask,
    Covered,
    Offset,
    CentroidOffset,
    MyIndex,
    InnerCoverage,
}

impl fmt::Display for PixVal {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PixVal::MsCount => write!(f, ".mscount"),
            PixVal::CovMask => write!(f, ".covmask"),
            PixVal::Covered => write!(f, ".covered"),
            PixVal::Offset => write!(f, ".offset"),
            PixVal::CentroidOffset => write!(f, ".centroid_offset"),
            PixVal::MyIndex => write!(f, ".my_index"),
            PixVal::InnerCoverage => write!(f, ".inner_coverage"),
        }
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpPixLd {
    pub dst: Dst,
    pub val: PixVal,
}

impl DisplayOp for OpPixLd {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "pixld{}", self.val)
    }
}
impl_display_for_op!(OpPixLd);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpS2R {
    pub dst: Dst,
    pub idx: u8,
}

impl DisplayOp for OpS2R {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "s2r sr[{:#x}]", self.idx)
    }
}
impl_display_for_op!(OpS2R);

pub enum VoteOp {
    Any,
    All,
    Eq,
}

impl fmt::Display for VoteOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            VoteOp::Any => write!(f, "any"),
            VoteOp::All => write!(f, "all"),
            VoteOp::Eq => write!(f, "eq"),
        }
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpVote {
    pub op: VoteOp,

    #[dst_type(GPR)]
    pub ballot: Dst,

    #[dst_type(Pred)]
    pub vote: Dst,

    #[src_type(Pred)]
    pub pred: Src,
}

impl DisplayOp for OpVote {
    fn fmt_dsts(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.ballot.is_none() && self.vote.is_none() {
            write!(f, "none")
        } else {
            if !self.ballot.is_none() {
                write!(f, "{}", self.ballot)?;
            }
            if !self.vote.is_none() {
                write!(f, "{}", self.vote)?;
            }
            Ok(())
        }
    }

    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "vote.{} {}", self.op, self.pred)
    }
}
impl_display_for_op!(OpVote);

#[allow(dead_code)]
#[derive(Copy, Clone)]
pub enum MatchOp {
    All,
    Any,
}

impl fmt::Display for MatchOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MatchOp::All => write!(f, ".all"),
            MatchOp::Any => write!(f, ".any"),
        }
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpMatch {
    #[dst_type(Pred)]
    pub pred: Dst,

    #[dst_type(GPR)]
    pub mask: Dst,

    #[src_type(GPR)]
    pub src: Src,

    pub op: MatchOp,
    pub u64: bool,
}

impl DisplayOp for OpMatch {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let u64_str = if self.u64 { ".u64" } else { "" };
        write!(f, "match{}{} {}", self.op, u64_str, self.src)
    }
}
impl_display_for_op!(OpMatch);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpUndef {
    pub dst: Dst,
}

impl DisplayOp for OpUndef {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "undef {}", self.dst)
    }
}
impl_display_for_op!(OpUndef);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSrcBar {
    pub src: Src,
}

impl DisplayOp for OpSrcBar {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "src_bar {}", self.src)
    }
}
impl_display_for_op!(OpSrcBar);

pub struct VecPair<A, B> {
    a: Vec<A>,
    b: Vec<B>,
}

impl<A, B> VecPair<A, B> {
    pub fn append(&mut self, other: &mut VecPair<A, B>) {
        self.a.append(&mut other.a);
        self.b.append(&mut other.b);
    }

    pub fn is_empty(&self) -> bool {
        debug_assert!(self.a.len() == self.b.len());
        self.a.is_empty()
    }

    pub fn iter(&self) -> Zip<slice::Iter<'_, A>, slice::Iter<'_, B>> {
        debug_assert!(self.a.len() == self.b.len());
        self.a.iter().zip(self.b.iter())
    }

    pub fn iter_mut(
        &mut self,
    ) -> Zip<slice::IterMut<'_, A>, slice::IterMut<'_, B>> {
        debug_assert!(self.a.len() == self.b.len());
        self.a.iter_mut().zip(self.b.iter_mut())
    }

    pub fn len(&self) -> usize {
        debug_assert!(self.a.len() == self.b.len());
        self.a.len()
    }

    pub fn new() -> Self {
        Self {
            a: Vec::new(),
            b: Vec::new(),
        }
    }

    pub fn push(&mut self, a: A, b: B) {
        debug_assert!(self.a.len() == self.b.len());
        self.a.push(a);
        self.b.push(b);
    }
}

impl<A: Clone, B: Clone> VecPair<A, B> {
    pub fn retain(&mut self, mut f: impl FnMut(&A, &B) -> bool) {
        debug_assert!(self.a.len() == self.b.len());
        let len = self.a.len();
        let mut i = 0_usize;
        while i < len {
            if !f(&self.a[i], &self.b[i]) {
                break;
            }
            i += 1;
        }

        let mut new_len = i;

        // Don't check this one twice.
        i += 1;

        while i < len {
            // This could be more efficient but it's good enough for our
            // purposes since everything we're storing is small and has a
            // trivial Drop.
            if f(&self.a[i], &self.b[i]) {
                self.a[new_len] = self.a[i].clone();
                self.b[new_len] = self.b[i].clone();
                new_len += 1;
            }
            i += 1;
        }

        if new_len < len {
            self.a.truncate(new_len);
            self.b.truncate(new_len);
        }
    }
}

mod phi {
    #[allow(unused_imports)]
    use crate::ir::{OpPhiDsts, OpPhiSrcs};
    use compiler::bitset::IntoBitIndex;
    use std::fmt;

    /// A phi node
    ///
    /// Phis in NAK are implemented differently from NIR and similar IRs.
    /// Instead of having a single phi instruction which lives in the successor
    /// block, each `Phi` represents a single merged 32-bit (or 1-bit for
    /// predicates) value and we have separate [`OpPhiSrcs`] and [`OpPhiDsts`]
    /// instructions which map phis to sources and destinations.
    ///
    /// One of the problems fundamental to phis is that they really live on the
    /// edges between blocks.  Regardless of where the phi instruction lives in
    /// the IR data structures, its sources are consumed at the end of the
    /// predecessor block and its destinations are defined at the start of the
    /// successor block and all phi sources and destinations get consumed and go
    /// live simultaneously for any given CFG edge.  For a phi that participates
    /// in a back-edge, this means that the source of the phi may be consumed
    /// after (in block order) the destination goes live.
    ///
    /// In NIR, this has caused no end of headaches.  Most passes which need to
    /// process phis ignore phis when first processing a block and then have a
    /// special case at the end of each block which walks the successors and
    /// processes the successor's phis, looking only at the phi sources whose
    /// predecessor matches the block.  This is clunky and often forgotten by
    /// optimization and lowering pass authors.  It's also easy to get missed by
    /// testing since it only really breaks if you have a phi which participates
    /// in a back-edge so it often gets found later when something breaks in the
    /// wild.
    ///
    /// To work around this (and also make things a little more Rust-friendly),
    /// NAK places the instruction which consumes phi sources at the end of the
    /// predecessor block and the instruction which defines phi destinations at
    /// the start of the successor block.  This structurally eliminates the
    /// problem that has plagued NIR for years.  The cost to this solution is
    /// that we have to create maps from phis to/from SSA values whenever we
    /// want to optimize the phis themselves.  However, this affects few enough
    /// passes that the benefits to the rest of the IR are worth the trade-off,
    /// at least for a back-end compiler.
    #[derive(Clone, Copy, Eq, Hash, PartialEq)]
    pub struct Phi {
        idx: u32,
    }

    impl IntoBitIndex for Phi {
        fn into_bit_index(self) -> usize {
            self.idx.try_into().unwrap()
        }
    }

    impl fmt::Display for Phi {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            write!(f, "φ{}", self.idx)
        }
    }

    pub struct PhiAllocator {
        count: u32,
    }

    impl PhiAllocator {
        pub fn new() -> PhiAllocator {
            PhiAllocator { count: 0 }
        }

        pub fn alloc(&mut self) -> Phi {
            let idx = self.count;
            self.count = idx + 1;
            Phi { idx }
        }
    }
}
pub use phi::{Phi, PhiAllocator};

/// An instruction which maps [Phi]s to sources in the predecessor block
#[repr(C)]
#[derive(DstsAsSlice)]
pub struct OpPhiSrcs {
    pub srcs: VecPair<Phi, Src>,
}

impl OpPhiSrcs {
    pub fn new() -> OpPhiSrcs {
        OpPhiSrcs {
            srcs: VecPair::new(),
        }
    }
}

impl AsSlice<Src> for OpPhiSrcs {
    type Attr = SrcType;

    fn as_slice(&self) -> &[Src] {
        &self.srcs.b
    }

    fn as_mut_slice(&mut self) -> &mut [Src] {
        &mut self.srcs.b
    }

    fn attrs(&self) -> SrcTypeList {
        SrcTypeList::Uniform(SrcType::GPR)
    }
}

impl DisplayOp for OpPhiSrcs {
    fn fmt_dsts(&self, _f: &mut fmt::Formatter<'_>) -> fmt::Result {
        Ok(())
    }

    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "phi_src ")?;
        for (i, (phi, src)) in self.srcs.iter().enumerate() {
            if i > 0 {
                write!(f, ", ")?;
            }
            write!(f, "{phi} = {src}")?;
        }
        Ok(())
    }
}
impl_display_for_op!(OpPhiSrcs);

/// An instruction which maps [Phi]s to destinations in the succeessor block
#[repr(C)]
#[derive(SrcsAsSlice)]
pub struct OpPhiDsts {
    pub dsts: VecPair<Phi, Dst>,
}

impl OpPhiDsts {
    pub fn new() -> OpPhiDsts {
        OpPhiDsts {
            dsts: VecPair::new(),
        }
    }
}

impl AsSlice<Dst> for OpPhiDsts {
    type Attr = DstType;

    fn as_slice(&self) -> &[Dst] {
        &self.dsts.b
    }

    fn as_mut_slice(&mut self) -> &mut [Dst] {
        &mut self.dsts.b
    }

    fn attrs(&self) -> DstTypeList {
        DstTypeList::Uniform(DstType::Vec)
    }
}

impl DisplayOp for OpPhiDsts {
    fn fmt_dsts(&self, _f: &mut fmt::Formatter<'_>) -> fmt::Result {
        Ok(())
    }

    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "phi_dst ")?;
        for (i, (phi, dst)) in self.dsts.iter().enumerate() {
            if i > 0 {
                write!(f, ", ")?;
            }
            write!(f, "{dst} = {phi}")?;
        }
        Ok(())
    }
}
impl_display_for_op!(OpPhiDsts);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpCopy {
    pub dst: Dst,
    pub src: Src,
}

impl DisplayOp for OpCopy {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "copy {}", self.src)
    }
}
impl_display_for_op!(OpCopy);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
/// Copies a value and pins its destination in the register file
pub struct OpPin {
    pub dst: Dst,
    #[src_type(SSA)]
    pub src: Src,
}

impl DisplayOp for OpPin {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "pin {}", self.src)
    }
}
impl_display_for_op!(OpPin);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
/// Copies a pinned value to an unpinned value
pub struct OpUnpin {
    pub dst: Dst,
    #[src_type(SSA)]
    pub src: Src,
}

impl DisplayOp for OpUnpin {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "unpin {}", self.src)
    }
}
impl_display_for_op!(OpUnpin);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpSwap {
    pub dsts: [Dst; 2],
    pub srcs: [Src; 2],
}

impl DisplayOp for OpSwap {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "swap {} {}", self.srcs[0], self.srcs[1])
    }
}
impl_display_for_op!(OpSwap);

#[repr(C)]
pub struct OpParCopy {
    pub dsts_srcs: VecPair<Dst, Src>,
    pub tmp: Option<RegRef>,
}

impl OpParCopy {
    pub fn new() -> OpParCopy {
        OpParCopy {
            dsts_srcs: VecPair::new(),
            tmp: None,
        }
    }

    pub fn is_empty(&self) -> bool {
        self.dsts_srcs.is_empty()
    }

    pub fn push(&mut self, dst: Dst, src: Src) {
        self.dsts_srcs.push(dst, src);
    }
}

impl AsSlice<Src> for OpParCopy {
    type Attr = SrcType;

    fn as_slice(&self) -> &[Src] {
        &self.dsts_srcs.b
    }

    fn as_mut_slice(&mut self) -> &mut [Src] {
        &mut self.dsts_srcs.b
    }

    fn attrs(&self) -> SrcTypeList {
        SrcTypeList::Uniform(SrcType::GPR)
    }
}

impl AsSlice<Dst> for OpParCopy {
    type Attr = DstType;

    fn as_slice(&self) -> &[Dst] {
        &self.dsts_srcs.a
    }

    fn as_mut_slice(&mut self) -> &mut [Dst] {
        &mut self.dsts_srcs.a
    }

    fn attrs(&self) -> DstTypeList {
        DstTypeList::Uniform(DstType::Vec)
    }
}

impl DisplayOp for OpParCopy {
    fn fmt_dsts(&self, _f: &mut fmt::Formatter<'_>) -> fmt::Result {
        Ok(())
    }

    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "par_copy")?;
        for (i, (dst, src)) in self.dsts_srcs.iter().enumerate() {
            if i > 0 {
                write!(f, ",")?;
            }
            write!(f, " {} = {}", dst, src)?;
        }
        Ok(())
    }
}
impl_display_for_op!(OpParCopy);

#[repr(C)]
#[derive(DstsAsSlice)]
pub struct OpRegOut {
    pub srcs: Vec<Src>,
}

impl AsSlice<Src> for OpRegOut {
    type Attr = SrcType;

    fn as_slice(&self) -> &[Src] {
        &self.srcs
    }

    fn as_mut_slice(&mut self) -> &mut [Src] {
        &mut self.srcs
    }

    fn attrs(&self) -> SrcTypeList {
        SrcTypeList::Uniform(SrcType::GPR)
    }
}

impl DisplayOp for OpRegOut {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "reg_out {{")?;
        for (i, src) in self.srcs.iter().enumerate() {
            if i > 0 {
                write!(f, ",")?;
            }
            write!(f, " {}", src)?;
        }
        write!(f, " }}")
    }
}
impl_display_for_op!(OpRegOut);

#[derive(Copy, Clone, Debug, PartialEq)]
pub enum OutType {
    Emit,
    Cut,
    EmitThenCut,
}

impl fmt::Display for OutType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            OutType::Emit => write!(f, "emit"),
            OutType::Cut => write!(f, "cut"),
            OutType::EmitThenCut => write!(f, "emit_then_cut"),
        }
    }
}

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpOut {
    pub dst: Dst,

    #[src_type(SSA)]
    pub handle: Src,

    #[src_type(ALU)]
    pub stream: Src,

    pub out_type: OutType,
}

impl DisplayOp for OpOut {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "out.{} {} {}", self.out_type, self.handle, self.stream)
    }
}
impl_display_for_op!(OpOut);

#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpOutFinal {
    #[src_type(SSA)]
    pub handle: Src,
}

impl DisplayOp for OpOutFinal {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "out.final {{ {} }}", self.handle)
    }
}
impl_display_for_op!(OpOutFinal);

/// Describes an annotation on an instruction.
#[repr(C)]
#[derive(SrcsAsSlice, DstsAsSlice)]
pub struct OpAnnotate {
    /// The annotation
    pub annotation: String,
}

impl DisplayOp for OpAnnotate {
    fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "// {}", self.annotation)
    }
}

impl fmt::Display for OpAnnotate {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.fmt_op(f)
    }
}

#[derive(DisplayOp, DstsAsSlice, SrcsAsSlice, FromVariants)]
pub enum Op {
    FAdd(OpFAdd),
    FFma(OpFFma),
    FMnMx(OpFMnMx),
    FMul(OpFMul),
    Rro(OpRro),
    MuFu(OpMuFu),
    FSet(OpFSet),
    FSetP(OpFSetP),
    FSwzAdd(OpFSwzAdd),
    FSwz(OpFSwz),
    DAdd(OpDAdd),
    DFma(OpDFma),
    DMnMx(OpDMnMx),
    DMul(OpDMul),
    DSetP(OpDSetP),
    HAdd2(OpHAdd2),
    HFma2(OpHFma2),
    HMul2(OpHMul2),
    HSet2(OpHSet2),
    HSetP2(OpHSetP2),
    Imma(OpImma),
    Hmma(OpHmma),
    HMnMx2(OpHMnMx2),
    BMsk(OpBMsk),
    BRev(OpBRev),
    Bfe(OpBfe),
    Flo(OpFlo),
    IAbs(OpIAbs),
    IAdd2(OpIAdd2),
    IAdd2X(OpIAdd2X),
    IAdd3(OpIAdd3),
    IAdd3X(OpIAdd3X),
    IDp4(OpIDp4),
    IMad(OpIMad),
    IMad64(OpIMad64),
    IMul(OpIMul),
    IMnMx(OpIMnMx),
    ISetP(OpISetP),
    Lea(OpLea),
    LeaX(OpLeaX),
    Lop2(OpLop2),
    Lop3(OpLop3),
    PopC(OpPopC),
    Shf(OpShf),
    Shl(OpShl),
    Shr(OpShr),
    F2F(OpF2F),
    F2FP(OpF2FP),
    F2I(OpF2I),
    I2F(OpI2F),
    I2I(OpI2I),
    FRnd(OpFRnd),
    Mov(OpMov),
    Prmt(OpPrmt),
    Sel(OpSel),
    Shfl(OpShfl),
    PLop3(OpPLop3),
    PSetP(OpPSetP),
    R2UR(OpR2UR),
    Redux(OpRedux),
    Tex(OpTex),
    Tld(OpTld),
    Tld4(OpTld4),
    Tmml(OpTmml),
    Txd(OpTxd),
    Txq(OpTxq),
    SuLd(OpSuLd),
    SuSt(OpSuSt),
    SuAtom(OpSuAtom),
    SuClamp(OpSuClamp),
    SuBfm(OpSuBfm),
    SuEau(OpSuEau),
    IMadSp(OpIMadSp),
    SuLdGa(OpSuLdGa),
    SuStGa(OpSuStGa),
    Ld(OpLd),
    Ldc(OpLdc),
    LdSharedLock(OpLdSharedLock),
    St(OpSt),
    StSCheckUnlock(OpStSCheckUnlock),
    Atom(OpAtom),
    AL2P(OpAL2P),
    ALd(OpALd),
    ASt(OpASt),
    Ipa(OpIpa),
    LdTram(OpLdTram),
    CCtl(OpCCtl),
    MemBar(OpMemBar),
    BClear(OpBClear),
    BMov(OpBMov),
    Break(OpBreak),
    BSSy(OpBSSy),
    BSync(OpBSync),
    Bra(OpBra),
    SSy(OpSSy),
    Sync(OpSync),
    Brk(OpBrk),
    PBk(OpPBk),
    Cont(OpCont),
    PCnt(OpPCnt),
    Exit(OpExit),
    WarpSync(OpWarpSync),
    Bar(OpBar),
    TexDepBar(OpTexDepBar),
    CS2R(OpCS2R),
    Isberd(OpIsberd),
    ViLd(OpViLd),
    Kill(OpKill),
    Nop(OpNop),
    PixLd(OpPixLd),
    S2R(OpS2R),
    Vote(OpVote),
    Match(OpMatch),
    Undef(OpUndef),
    SrcBar(OpSrcBar),
    PhiSrcs(OpPhiSrcs),
    PhiDsts(OpPhiDsts),
    Copy(OpCopy),
    Pin(OpPin),
    Unpin(OpUnpin),
    Swap(OpSwap),
    ParCopy(OpParCopy),
    RegOut(OpRegOut),
    Out(OpOut),
    OutFinal(OpOutFinal),
    Annotate(OpAnnotate),
}
impl_display_for_op!(Op);

impl Op {
    pub fn is_branch(&self) -> bool {
        match self {
            Op::Bra(_)
            | Op::Sync(_)
            | Op::Brk(_)
            | Op::Cont(_)
            | Op::Exit(_) => true,
            _ => false,
        }
    }

    pub fn is_fp64(&self) -> bool {
        match self {
            Op::MuFu(op) => matches!(op.op, MuFuOp::Rcp64H | MuFuOp::Rsq64H),
            Op::DAdd(_)
            | Op::DFma(_)
            | Op::DMnMx(_)
            | Op::DMul(_)
            | Op::DSetP(_) => true,
            Op::F2F(op) => op.src_type.bits() == 64 || op.dst_type.bits() == 64,
            Op::F2I(op) => op.src_type.bits() == 64 || op.dst_type.bits() == 64,
            Op::I2F(op) => op.src_type.bits() == 64 || op.dst_type.bits() == 64,
            Op::FRnd(op) => {
                op.src_type.bits() == 64 || op.dst_type.bits() == 64
            }
            _ => false,
        }
    }

    pub fn has_fixed_latency(&self, sm: u8) -> bool {
        match self {
            // Float ALU
            Op::F2FP(_)
            | Op::FAdd(_)
            | Op::FFma(_)
            | Op::FMnMx(_)
            | Op::FMul(_)
            | Op::FSet(_)
            | Op::FSetP(_)
            | Op::HAdd2(_)
            | Op::HFma2(_)
            | Op::HMul2(_)
            | Op::HSet2(_)
            | Op::HSetP2(_)
            | Op::HMnMx2(_)
            | Op::FSwz(_)
            | Op::FSwzAdd(_) => true,

            // Multi-function unit is variable latency
            Op::Rro(_) | Op::MuFu(_) => false,

            // Double-precision float ALU
            Op::DAdd(_)
            | Op::DFma(_)
            | Op::DMnMx(_)
            | Op::DMul(_)
            | Op::DSetP(_) => false,

            // Matrix Multiply Add
            Op::Imma(_) | Op::Hmma(_) => false,

            // Integer ALU
            Op::BRev(_) | Op::Flo(_) | Op::PopC(_) => false,
            Op::IMad(_) | Op::IMul(_) => sm >= 70,
            Op::BMsk(_)
            | Op::IAbs(_)
            | Op::IAdd2(_)
            | Op::IAdd2X(_)
            | Op::IAdd3(_)
            | Op::IAdd3X(_)
            | Op::IDp4(_)
            | Op::IMad64(_)
            | Op::IMnMx(_)
            | Op::ISetP(_)
            | Op::Lea(_)
            | Op::LeaX(_)
            | Op::Lop2(_)
            | Op::Lop3(_)
            | Op::SuClamp(_)
            | Op::SuBfm(_)
            | Op::SuEau(_)
            | Op::IMadSp(_)
            | Op::Shf(_)
            | Op::Shl(_)
            | Op::Shr(_)
            | Op::Bfe(_) => true,

            // Conversions are variable latency?!?
            Op::F2F(_) | Op::F2I(_) | Op::I2F(_) | Op::I2I(_) | Op::FRnd(_) => {
                false
            }

            // Move ops
            Op::Mov(_) | Op::Prmt(_) | Op::Sel(_) => true,
            Op::Shfl(_) => false,

            // Predicate ops
            Op::PLop3(_) | Op::PSetP(_) => true,

            // Uniform ops
            Op::R2UR(_) | Op::Redux(_) => false,

            // Texture ops
            Op::Tex(_)
            | Op::Tld(_)
            | Op::Tld4(_)
            | Op::Tmml(_)
            | Op::Txd(_)
            | Op::Txq(_) => false,

            // Surface ops
            Op::SuLd(_)
            | Op::SuSt(_)
            | Op::SuAtom(_)
            | Op::SuLdGa(_)
            | Op::SuStGa(_) => false,

            // Memory ops
            Op::Ld(_)
            | Op::Ldc(_)
            | Op::LdSharedLock(_)
            | Op::St(_)
            | Op::StSCheckUnlock(_)
            | Op::Atom(_)
            | Op::AL2P(_)
            | Op::ALd(_)
            | Op::ASt(_)
            | Op::Ipa(_)
            | Op::CCtl(_)
            | Op::LdTram(_)
            | Op::MemBar(_) => false,

            // Control-flow ops
            Op::BClear(_)
            | Op::Break(_)
            | Op::BSSy(_)
            | Op::BSync(_)
            | Op::SSy(_)
            | Op::Sync(_)
            | Op::Brk(_)
            | Op::PBk(_)
            | Op::Cont(_)
            | Op::PCnt(_)
            | Op::Bra(_)
            | Op::Exit(_)
            | Op::WarpSync(_) => false,

            // The barrier half is HW scoreboarded by the GPR isn't.  When
            // moving from a GPR to a barrier, we still need a token for WaR
            // hazards.
            Op::BMov(_) => false,

            // Geometry ops
            Op::Out(_) | Op::OutFinal(_) => false,

            // Miscellaneous ops
            Op::Bar(_)
            | Op::TexDepBar(_)
            | Op::CS2R(_)
            | Op::Isberd(_)
            | Op::ViLd(_)
            | Op::Kill(_)
            | Op::PixLd(_)
            | Op::S2R(_)
            | Op::Match(_) => false,
            Op::Nop(_) | Op::Vote(_) => true,

            // Virtual ops
            Op::Undef(_)
            | Op::SrcBar(_)
            | Op::PhiSrcs(_)
            | Op::PhiDsts(_)
            | Op::Copy(_)
            | Op::Pin(_)
            | Op::Unpin(_)
            | Op::Swap(_)
            | Op::ParCopy(_)
            | Op::RegOut(_)
            | Op::Annotate(_) => {
                panic!("Not a hardware opcode")
            }
        }
    }

    /// Some decoupled instructions don't need
    /// scoreboards, due to our usage.
    pub fn no_scoreboard(&self) -> bool {
        match self {
            Op::BClear(_)
            | Op::Break(_)
            | Op::BSSy(_)
            | Op::BSync(_)
            | Op::SSy(_)
            | Op::Sync(_)
            | Op::Brk(_)
            | Op::PBk(_)
            | Op::Cont(_)
            | Op::PCnt(_)
            | Op::Bra(_)
            | Op::Exit(_) => true,
            _ => false,
        }
    }
}

#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub enum PredRef {
    None,
    SSA(SSAValue),
    Reg(RegRef),
}

impl PredRef {
    #[allow(dead_code)]
    pub fn as_reg(&self) -> Option<&RegRef> {
        match self {
            PredRef::Reg(r) => Some(r),
            _ => None,
        }
    }

    #[allow(dead_code)]
    pub fn as_ssa(&self) -> Option<&SSAValue> {
        match self {
            PredRef::SSA(r) => Some(r),
            _ => None,
        }
    }

    pub fn is_none(&self) -> bool {
        matches!(self, PredRef::None)
    }

    pub fn iter_ssa(&self) -> slice::Iter<'_, SSAValue> {
        match self {
            PredRef::None | PredRef::Reg(_) => &[],
            PredRef::SSA(ssa) => slice::from_ref(ssa),
        }
        .iter()
    }

    pub fn iter_ssa_mut(&mut self) -> slice::IterMut<'_, SSAValue> {
        match self {
            PredRef::None | PredRef::Reg(_) => &mut [],
            PredRef::SSA(ssa) => slice::from_mut(ssa),
        }
        .iter_mut()
    }
}

impl From<RegRef> for PredRef {
    fn from(reg: RegRef) -> PredRef {
        PredRef::Reg(reg)
    }
}

impl From<SSAValue> for PredRef {
    fn from(ssa: SSAValue) -> PredRef {
        PredRef::SSA(ssa)
    }
}

impl fmt::Display for PredRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PredRef::None => write!(f, "pT"),
            PredRef::SSA(ssa) => ssa.fmt(f),
            PredRef::Reg(reg) => reg.fmt(f),
        }
    }
}

#[derive(Clone, Copy)]
pub struct Pred {
    pub pred_ref: PredRef,
    pub pred_inv: bool,
}

impl Pred {
    pub fn is_true(&self) -> bool {
        self.pred_ref.is_none() && !self.pred_inv
    }

    pub fn is_false(&self) -> bool {
        self.pred_ref.is_none() && self.pred_inv
    }

    pub fn iter_ssa(&self) -> slice::Iter<'_, SSAValue> {
        self.pred_ref.iter_ssa()
    }

    pub fn iter_ssa_mut(&mut self) -> slice::IterMut<'_, SSAValue> {
        self.pred_ref.iter_ssa_mut()
    }

    pub fn bnot(self) -> Self {
        Pred {
            pred_ref: self.pred_ref,
            pred_inv: !self.pred_inv,
        }
    }
}

impl From<bool> for Pred {
    fn from(b: bool) -> Self {
        Pred {
            pred_ref: PredRef::None,
            pred_inv: !b,
        }
    }
}

impl<T: Into<PredRef>> From<T> for Pred {
    fn from(p: T) -> Self {
        Pred {
            pred_ref: p.into(),
            pred_inv: false,
        }
    }
}

impl fmt::Display for Pred {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.pred_inv {
            write!(f, "!")?;
        }
        self.pred_ref.fmt(f)
    }
}

pub const MIN_INSTR_DELAY: u8 = 1;

pub struct InstrDeps {
    pub delay: u8,
    pub yld: bool,
    wr_bar: i8,
    rd_bar: i8,
    pub wt_bar_mask: u8,
    pub reuse_mask: u8,
}

impl InstrDeps {
    pub fn new() -> InstrDeps {
        InstrDeps {
            delay: 0,
            yld: false,
            wr_bar: -1,
            rd_bar: -1,
            wt_bar_mask: 0,
            reuse_mask: 0,
        }
    }

    pub fn rd_bar(&self) -> Option<u8> {
        if self.rd_bar < 0 {
            None
        } else {
            Some(self.rd_bar.try_into().unwrap())
        }
    }

    pub fn wr_bar(&self) -> Option<u8> {
        if self.wr_bar < 0 {
            None
        } else {
            Some(self.wr_bar.try_into().unwrap())
        }
    }

    pub fn set_delay(&mut self, delay: u8) {
        self.delay = delay;
    }

    pub fn set_yield(&mut self, yld: bool) {
        self.yld = yld;
    }

    pub fn set_rd_bar(&mut self, idx: u8) {
        assert!(idx < 6);
        self.rd_bar = idx.try_into().unwrap();
    }

    pub fn set_wr_bar(&mut self, idx: u8) {
        assert!(idx < 6);
        self.wr_bar = idx.try_into().unwrap();
    }

    pub fn add_wt_bar(&mut self, idx: u8) {
        self.add_wt_bar_mask(1 << idx);
    }

    pub fn add_wt_bar_mask(&mut self, bar_mask: u8) {
        assert!(bar_mask < 1 << 6);
        self.wt_bar_mask |= bar_mask;
    }

    #[allow(dead_code)]
    pub fn add_reuse(&mut self, idx: u8) {
        assert!(idx < 6);
        self.reuse_mask |= 1_u8 << idx;
    }
}

impl fmt::Display for InstrDeps {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.delay > 0 {
            write!(f, " delay={}", self.delay)?;
        }
        if self.wt_bar_mask != 0 {
            write!(f, " wt={:06b}", self.wt_bar_mask)?;
        }
        if self.rd_bar >= 0 {
            write!(f, " rd:{}", self.rd_bar)?;
        }
        if self.wr_bar >= 0 {
            write!(f, " wr:{}", self.wr_bar)?;
        }
        if self.reuse_mask != 0 {
            write!(f, " reuse={:06b}", self.reuse_mask)?;
        }
        if self.yld {
            write!(f, " yld")?;
        }
        Ok(())
    }
}

pub struct Instr {
    pub pred: Pred,
    pub op: Op,
    pub deps: InstrDeps,
}

impl Instr {
    pub fn new(op: impl Into<Op>) -> Instr {
        Instr {
            op: op.into(),
            pred: true.into(),
            deps: InstrDeps::new(),
        }
    }

    pub fn new_boxed(op: impl Into<Op>) -> Box<Self> {
        Box::new(Instr::new(op))
    }

    pub fn dsts(&self) -> &[Dst] {
        self.op.dsts_as_slice()
    }

    pub fn dsts_mut(&mut self) -> &mut [Dst] {
        self.op.dsts_as_mut_slice()
    }

    pub fn srcs(&self) -> &[Src] {
        self.op.srcs_as_slice()
    }

    pub fn srcs_mut(&mut self) -> &mut [Src] {
        self.op.srcs_as_mut_slice()
    }

    pub fn src_types(&self) -> SrcTypeList {
        self.op.src_types()
    }

    pub fn for_each_ssa_use(&self, mut f: impl FnMut(&SSAValue)) {
        for ssa in self.pred.iter_ssa() {
            f(ssa);
        }
        for src in self.srcs() {
            for ssa in src.iter_ssa() {
                f(ssa);
            }
        }
    }

    pub fn for_each_ssa_use_mut(&mut self, mut f: impl FnMut(&mut SSAValue)) {
        for ssa in self.pred.iter_ssa_mut() {
            f(ssa);
        }
        for src in self.srcs_mut() {
            for ssa in src.iter_ssa_mut() {
                f(ssa);
            }
        }
    }

    pub fn for_each_ssa_def(&self, mut f: impl FnMut(&SSAValue)) {
        for dst in self.dsts() {
            for ssa in dst.iter_ssa() {
                f(ssa);
            }
        }
    }

    pub fn for_each_ssa_def_mut(&mut self, mut f: impl FnMut(&mut SSAValue)) {
        for dst in self.dsts_mut() {
            for ssa in dst.iter_ssa_mut() {
                f(ssa);
            }
        }
    }

    pub fn is_branch(&self) -> bool {
        self.op.is_branch()
    }

    pub fn uses_global_mem(&self) -> bool {
        match &self.op {
            Op::Atom(op) => op.mem_space != MemSpace::Local,
            Op::Ld(op) => op.access.space != MemSpace::Local,
            Op::St(op) => op.access.space != MemSpace::Local,
            Op::SuAtom(_)
            | Op::SuLd(_)
            | Op::SuSt(_)
            | Op::SuLdGa(_)
            | Op::SuStGa(_) => true,
            _ => false,
        }
    }

    pub fn writes_global_mem(&self) -> bool {
        match &self.op {
            Op::Atom(op) => matches!(op.mem_space, MemSpace::Global(_)),
            Op::St(op) => matches!(op.access.space, MemSpace::Global(_)),
            Op::SuAtom(_) | Op::SuSt(_) | Op::SuStGa(_) => true,
            _ => false,
        }
    }

    pub fn can_eliminate(&self) -> bool {
        match &self.op {
            Op::ASt(_)
            | Op::SuSt(_)
            | Op::SuStGa(_)
            | Op::SuAtom(_)
            | Op::LdSharedLock(_)
            | Op::St(_)
            | Op::StSCheckUnlock(_)
            | Op::Atom(_)
            | Op::CCtl(_)
            | Op::MemBar(_)
            | Op::Kill(_)
            | Op::Nop(_)
            | Op::BSync(_)
            | Op::Bra(_)
            | Op::SSy(_)
            | Op::Sync(_)
            | Op::Brk(_)
            | Op::PBk(_)
            | Op::Cont(_)
            | Op::PCnt(_)
            | Op::Exit(_)
            | Op::WarpSync(_)
            | Op::Bar(_)
            | Op::TexDepBar(_)
            | Op::RegOut(_)
            | Op::Out(_)
            | Op::OutFinal(_)
            | Op::Annotate(_) => false,
            Op::BMov(op) => !op.clear,
            _ => true,
        }
    }

    pub fn is_uniform(&self) -> bool {
        match &self.op {
            Op::PhiDsts(_) => false,
            op => op.is_uniform(),
        }
    }

    pub fn needs_yield(&self) -> bool {
        matches!(&self.op, Op::Bar(_) | Op::BSync(_))
    }

    fn fmt_pred(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if !self.pred.is_true() {
            write!(f, "@{} ", self.pred)?;
        }
        Ok(())
    }
}

impl fmt::Display for Instr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} {}{}", Fmt(|f| self.fmt_pred(f)), self.op, self.deps)
    }
}

impl<T: Into<Op>> From<T> for Instr {
    fn from(value: T) -> Self {
        Self::new(value)
    }
}

pub type MappedInstrs = SmallVec<Box<Instr>>;

pub struct BasicBlock {
    pub label: Label,

    /// Whether or not this block is uniform
    ///
    /// If true, then all non-exited lanes in a warp which execute this block
    /// are guaranteed to execute it together
    pub uniform: bool,

    pub instrs: Vec<Box<Instr>>,
}

impl BasicBlock {
    pub fn map_instrs(
        &mut self,
        mut map: impl FnMut(Box<Instr>) -> MappedInstrs,
    ) {
        let mut instrs = Vec::new();
        for i in self.instrs.drain(..) {
            match map(i) {
                MappedInstrs::None => (),
                MappedInstrs::One(i) => {
                    instrs.push(i);
                }
                MappedInstrs::Many(mut v) => {
                    instrs.append(&mut v);
                }
            }
        }
        self.instrs = instrs;
    }

    pub fn phi_dsts_ip(&self) -> Option<usize> {
        for (ip, instr) in self.instrs.iter().enumerate() {
            match &instr.op {
                Op::Annotate(_) => (),
                Op::PhiDsts(_) => return Some(ip),
                _ => break,
            }
        }
        None
    }

    pub fn phi_dsts(&self) -> Option<&OpPhiDsts> {
        self.phi_dsts_ip().map(|ip| match &self.instrs[ip].op {
            Op::PhiDsts(phi) => phi,
            _ => panic!("Expected to find the phi"),
        })
    }

    #[allow(dead_code)]
    pub fn phi_dsts_mut(&mut self) -> Option<&mut OpPhiDsts> {
        self.phi_dsts_ip().map(|ip| match &mut self.instrs[ip].op {
            Op::PhiDsts(phi) => phi,
            _ => panic!("Expected to find the phi"),
        })
    }

    pub fn phi_srcs_ip(&self) -> Option<usize> {
        for (ip, instr) in self.instrs.iter().enumerate().rev() {
            match &instr.op {
                Op::Annotate(_) => (),
                Op::PhiSrcs(_) => return Some(ip),
                _ if instr.is_branch() => (),
                _ => break,
            }
        }
        None
    }
    pub fn phi_srcs(&self) -> Option<&OpPhiSrcs> {
        self.phi_srcs_ip().map(|ip| match &self.instrs[ip].op {
            Op::PhiSrcs(phi) => phi,
            _ => panic!("Expected to find the phi"),
        })
    }

    pub fn phi_srcs_mut(&mut self) -> Option<&mut OpPhiSrcs> {
        self.phi_srcs_ip().map(|ip| match &mut self.instrs[ip].op {
            Op::PhiSrcs(phi) => phi,
            _ => panic!("Expected to find the phi"),
        })
    }

    pub fn branch(&self) -> Option<&Instr> {
        if let Some(i) = self.instrs.last() {
            if i.is_branch() {
                Some(i)
            } else {
                None
            }
        } else {
            None
        }
    }

    pub fn branch_ip(&self) -> Option<usize> {
        if let Some(i) = self.instrs.last() {
            if i.is_branch() {
                Some(self.instrs.len() - 1)
            } else {
                None
            }
        } else {
            None
        }
    }

    #[allow(dead_code)]
    pub fn branch_mut(&mut self) -> Option<&mut Instr> {
        if let Some(i) = self.instrs.last_mut() {
            if i.is_branch() {
                Some(i)
            } else {
                None
            }
        } else {
            None
        }
    }

    pub fn falls_through(&self) -> bool {
        if let Some(i) = self.branch() {
            !i.pred.is_true()
        } else {
            true
        }
    }
}

pub struct Function {
    pub ssa_alloc: SSAValueAllocator,
    pub phi_alloc: PhiAllocator,
    pub blocks: CFG<BasicBlock>,
}

impl Function {
    pub fn map_instrs(
        &mut self,
        mut map: impl FnMut(Box<Instr>, &mut SSAValueAllocator) -> MappedInstrs,
    ) {
        let alloc = &mut self.ssa_alloc;
        for b in &mut self.blocks {
            b.map_instrs(|i| map(i, alloc));
        }
    }
}

impl fmt::Display for Function {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut pred_width = 0;
        let mut dsts_width = 0;
        let mut op_width = 0;

        let mut blocks = Vec::new();
        for b in &self.blocks {
            let mut instrs = Vec::new();
            for i in &b.instrs {
                let mut pred = String::new();
                write!(pred, "{}", Fmt(|f| i.fmt_pred(f)))?;
                let mut dsts = String::new();
                write!(dsts, "{}", Fmt(|f| i.op.fmt_dsts(f)))?;
                let mut op = String::new();
                write!(op, "{}", Fmt(|f| i.op.fmt_op(f)))?;
                let mut deps = String::new();
                write!(deps, "{}", i.deps)?;

                pred_width = max(pred_width, pred.len());
                dsts_width = max(dsts_width, dsts.len());
                op_width = max(op_width, op.len());
                let is_annotation = matches!(i.op, Op::Annotate(_));

                instrs.push((pred, dsts, op, deps, is_annotation));
            }
            blocks.push(instrs);
        }

        for (i, mut b) in blocks.drain(..).enumerate() {
            let u = if self.blocks[i].uniform { ".u" } else { "" };
            write!(f, "block{u} {} {} [", i, self.blocks[i].label)?;
            for (pi, p) in self.blocks.pred_indices(i).iter().enumerate() {
                if pi > 0 {
                    write!(f, ", ")?;
                }
                write!(f, "{}", p)?;
            }
            write!(f, "] -> {{\n")?;

            for (pred, dsts, op, deps, is_annotation) in b.drain(..) {
                let eq_sym = if dsts.is_empty() { " " } else { "=" };
                if is_annotation {
                    write!(f, "\n{}\n", op)?;
                } else if deps.is_empty() {
                    write!(
                        f,
                        "{:<pred_width$} {:<dsts_width$} {} {}\n",
                        pred, dsts, eq_sym, op,
                    )?;
                } else {
                    write!(
                        f,
                        "{:<pred_width$} {:<dsts_width$} {} \
                         {:<op_width$} //{}\n",
                        pred, dsts, eq_sym, op, deps,
                    )?;
                }
            }

            write!(f, "}} -> [")?;
            for (si, s) in self.blocks.succ_indices(i).iter().enumerate() {
                if si > 0 {
                    write!(f, ", ")?;
                }
                write!(f, "{}", s)?;
            }
            write!(f, "]\n")?;
        }
        Ok(())
    }
}

#[derive(Debug)]
pub struct ComputeShaderInfo {
    pub local_size: [u16; 3],
    pub smem_size: u16,
}

#[derive(Debug)]
pub struct FragmentShaderInfo {
    pub uses_kill: bool,
    pub does_interlock: bool,
    pub post_depth_coverage: bool,
    pub early_fragment_tests: bool,
    pub uses_sample_shading: bool,
}

#[derive(Debug)]
pub struct GeometryShaderInfo {
    pub passthrough_enable: bool,
    pub stream_out_mask: u8,
    pub threads_per_input_primitive: u8,
    pub output_topology: OutputTopology,
    pub max_output_vertex_count: u16,
}

impl Default for GeometryShaderInfo {
    fn default() -> Self {
        Self {
            passthrough_enable: false,
            stream_out_mask: 0,
            threads_per_input_primitive: 0,
            output_topology: OutputTopology::LineStrip,
            max_output_vertex_count: 0,
        }
    }
}

#[derive(Debug)]
pub struct TessellationInitShaderInfo {
    pub per_patch_attribute_count: u8,
    pub threads_per_patch: u8,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug)]
pub enum TessellationDomain {
    Isoline = NAK_TS_DOMAIN_ISOLINE,
    Triangle = NAK_TS_DOMAIN_TRIANGLE,
    Quad = NAK_TS_DOMAIN_QUAD,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug)]
pub enum TessellationSpacing {
    Integer = NAK_TS_SPACING_INTEGER,
    FractionalOdd = NAK_TS_SPACING_FRACT_ODD,
    FractionalEven = NAK_TS_SPACING_FRACT_EVEN,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug)]
pub enum TessellationPrimitives {
    Points = NAK_TS_PRIMS_POINTS,
    Lines = NAK_TS_PRIMS_LINES,
    TrianglesCW = NAK_TS_PRIMS_TRIANGLES_CW,
    TrianglesCCW = NAK_TS_PRIMS_TRIANGLES_CCW,
}

#[derive(Debug)]
pub struct TessellationShaderInfo {
    pub domain: TessellationDomain,
    pub spacing: TessellationSpacing,
    pub primitives: TessellationPrimitives,
}

#[derive(Debug)]
pub enum ShaderStageInfo {
    Compute(ComputeShaderInfo),
    Vertex,
    Fragment(FragmentShaderInfo),
    Geometry(GeometryShaderInfo),
    TessellationInit(TessellationInitShaderInfo),
    Tessellation(TessellationShaderInfo),
}

#[derive(Debug, Default)]
pub struct SysValInfo {
    pub ab: u32,
    pub c: u16,
}

#[derive(Debug)]
pub struct VtgIoInfo {
    pub sysvals_in: SysValInfo,
    pub sysvals_in_d: u8,
    pub sysvals_out: SysValInfo,
    pub sysvals_out_d: u8,
    pub attr_in: [u32; 4],
    pub attr_out: [u32; 4],
    pub store_req_start: u8,
    pub store_req_end: u8,
    pub clip_enable: u8,
    pub cull_enable: u8,
    pub xfb: Option<Box<nak_xfb_info>>,
}

impl VtgIoInfo {
    fn mark_attrs(&mut self, addrs: Range<u16>, written: bool) {
        let sysvals = if written {
            &mut self.sysvals_out
        } else {
            &mut self.sysvals_in
        };

        let sysvals_d = if written {
            &mut self.sysvals_out_d
        } else {
            &mut self.sysvals_in_d
        };

        let mut attr = BitMutView::new(if written {
            &mut self.attr_out
        } else {
            &mut self.attr_in
        });

        let mut addrs = addrs;
        addrs.start &= !3;
        for addr in addrs.step_by(4) {
            if addr < 0x080 {
                sysvals.ab |= 1 << (addr / 4);
            } else if addr < 0x280 {
                let attr_idx = (addr - 0x080) as usize / 4;
                attr.set_bit(attr_idx, true);
            } else if addr < 0x2c0 {
                panic!("FF color I/O not supported");
            } else if addr < 0x300 {
                sysvals.c |= 1 << ((addr - 0x2c0) / 4);
            } else if addr >= 0x3a0 && addr < 0x3c0 {
                *sysvals_d |= 1 << ((addr - 0x3a0) / 4);
            }
        }
    }

    pub fn mark_attrs_read(&mut self, addrs: Range<u16>) {
        self.mark_attrs(addrs, false);
    }

    pub fn mark_attrs_written(&mut self, addrs: Range<u16>) {
        self.mark_attrs(addrs, true);
    }

    pub fn attr_written(&self, addr: u16) -> bool {
        if addr < 0x080 {
            self.sysvals_out.ab & (1 << (addr / 4)) != 0
        } else if addr < 0x280 {
            let attr_idx = (addr - 0x080) as usize / 4;
            BitView::new(&self.attr_out).get_bit(attr_idx)
        } else if addr < 0x2c0 {
            panic!("FF color I/O not supported");
        } else if addr < 0x300 {
            self.sysvals_out.c & (1 << ((addr - 0x2c0) / 4)) != 0
        } else if addr >= 0x3a0 && addr < 0x3c0 {
            self.sysvals_out_d & (1 << ((addr - 0x3a0) / 4)) != 0
        } else {
            panic!("Unknown I/O address");
        }
    }

    pub fn mark_store_req(&mut self, addrs: Range<u16>) {
        let start = (addrs.start / 4).try_into().unwrap();
        let end = ((addrs.end - 1) / 4).try_into().unwrap();
        self.store_req_start = min(self.store_req_start, start);
        self.store_req_end = max(self.store_req_end, end);
    }
}

#[derive(Debug)]
pub struct FragmentIoInfo {
    pub sysvals_in: SysValInfo,
    pub sysvals_in_d: [PixelImap; 8],
    pub attr_in: [PixelImap; 128],
    pub barycentric_attr_in: [u32; 4],

    pub reads_sample_mask: bool,
    pub writes_color: u32,
    pub writes_sample_mask: bool,
    pub writes_depth: bool,
}

impl FragmentIoInfo {
    pub fn mark_attr_read(&mut self, addr: u16, interp: PixelImap) {
        if addr < 0x080 {
            self.sysvals_in.ab |= 1 << (addr / 4);
        } else if addr < 0x280 {
            let attr_idx = (addr - 0x080) as usize / 4;
            self.attr_in[attr_idx] = interp;
        } else if addr < 0x2c0 {
            panic!("FF color I/O not supported");
        } else if addr < 0x300 {
            self.sysvals_in.c |= 1 << ((addr - 0x2c0) / 4);
        } else if addr >= 0x3a0 && addr < 0x3c0 {
            let attr_idx = (addr - 0x3a0) as usize / 4;
            self.sysvals_in_d[attr_idx] = interp;
        }
    }

    pub fn mark_barycentric_attr_in(&mut self, addr: u16) {
        assert!(addr >= 0x80 && addr < 0x280);

        let mut attr = BitMutView::new(&mut self.barycentric_attr_in);

        let attr_idx = (addr - 0x080) as usize / 4;
        attr.set_bit(attr_idx, true);
    }
}

#[derive(Debug)]
pub enum ShaderIoInfo {
    None,
    Vtg(VtgIoInfo),
    Fragment(FragmentIoInfo),
}

#[derive(Debug)]
pub struct ShaderInfo {
    pub max_warps_per_sm: u32,
    pub num_gprs: u8,
    pub num_control_barriers: u8,
    pub num_instrs: u32,
    pub num_static_cycles: u32,
    pub num_spills_to_mem: u32,
    pub num_fills_from_mem: u32,
    pub num_spills_to_reg: u32,
    pub num_fills_from_reg: u32,
    pub slm_size: u32,
    pub max_crs_depth: u32,
    pub uses_global_mem: bool,
    pub writes_global_mem: bool,
    pub uses_fp64: bool,
    pub stage: ShaderStageInfo,
    pub io: ShaderIoInfo,
}

pub trait ShaderModel {
    fn sm(&self) -> u8;

    #[allow(dead_code)]
    fn is_fermi(&self) -> bool {
        self.sm() >= 20 && self.sm() < 30
    }

    #[allow(dead_code)]
    fn is_kepler_a(&self) -> bool {
        self.sm() >= 30 && self.sm() < 32
    }

    #[allow(dead_code)]
    fn is_kepler_b(&self) -> bool {
        // TK1 is SM 3.2 and desktop Kepler B is SM 3.3+
        self.sm() >= 32 && self.sm() < 40
    }

    #[allow(dead_code)]
    fn is_kepler(&self) -> bool {
        self.is_kepler_a() || self.is_kepler_b()
    }

    // The following helpers are pulled from GetSpaVersion in the open-source
    // NVIDIA kernel driver sources

    #[allow(dead_code)]
    fn is_maxwell(&self) -> bool {
        self.sm() >= 50 && self.sm() < 60
    }

    #[allow(dead_code)]
    fn is_pascal(&self) -> bool {
        self.sm() >= 60 && self.sm() < 70
    }

    #[allow(dead_code)]
    fn is_volta(&self) -> bool {
        self.sm() >= 70 && self.sm() < 73
    }

    #[allow(dead_code)]
    fn is_turing(&self) -> bool {
        self.sm() >= 73 && self.sm() < 80
    }

    #[allow(dead_code)]
    fn is_ampere(&self) -> bool {
        self.sm() >= 80 && self.sm() < 89
    }

    #[allow(dead_code)]
    fn is_ada(&self) -> bool {
        self.sm() == 89
    }

    #[allow(dead_code)]
    fn is_hopper(&self) -> bool {
        self.sm() >= 90 && self.sm() < 100
    }

    #[allow(dead_code)]
    fn is_blackwell_a(&self) -> bool {
        self.sm() >= 100 && self.sm() < 110
    }

    #[allow(dead_code)]
    fn is_blackwell_b(&self) -> bool {
        self.sm() >= 120 && self.sm() < 130
    }

    #[allow(dead_code)]
    fn is_blackwell(&self) -> bool {
        self.is_blackwell_a() || self.is_blackwell_b()
    }

    fn num_regs(&self, file: RegFile) -> u32;
    fn hw_reserved_gprs(&self) -> u32;
    fn crs_size(&self, max_crs_depth: u32) -> u32;

    fn op_can_be_uniform(&self, op: &Op) -> bool;

    // Scheduling information
    fn op_needs_scoreboard(&self, op: &Op) -> bool {
        !op.no_scoreboard() && !op.has_fixed_latency(self.sm())
    }

    /// Latency before another non-NOP can execute
    fn exec_latency(&self, op: &Op) -> u32;

    /// Read-after-read latency
    fn raw_latency(
        &self,
        write: &Op,
        dst_idx: usize,
        read: &Op,
        src_idx: usize,
    ) -> u32;

    /// Write-after-read latency
    fn war_latency(
        &self,
        read: &Op,
        src_idx: usize,
        write: &Op,
        dst_idx: usize,
    ) -> u32;

    /// Write-after-write latency
    fn waw_latency(
        &self,
        a: &Op,
        a_dst_idx: usize,
        a_has_pred: bool,
        b: &Op,
        b_dst_idx: usize,
    ) -> u32;

    /// Predicate read-after-write latency
    fn paw_latency(&self, write: &Op, dst_idx: usize) -> u32;

    /// Worst-case access-after-write latency
    fn worst_latency(&self, write: &Op, dst_idx: usize) -> u32;

    /// Maximum encodable instruction delay
    fn max_instr_delay(&self) -> u8;

    fn legalize_op(&self, b: &mut LegalizeBuilder, op: &mut Op);
    fn encode_shader(&self, s: &Shader<'_>) -> Vec<u32>;
}

/// For compute shaders, large values of local_size impose an additional limit
/// on the number of GPRs per thread
pub fn gpr_limit_from_local_size(local_size: &[u16; 3]) -> u32 {
    fn prev_multiple_of(x: u32, y: u32) -> u32 {
        (x / y) * y
    }

    let local_size = local_size[0] * local_size[1] * local_size[2];
    // Warps are allocated in multiples of 4
    // Multiply that by 32 threads/warp
    let local_size = local_size.next_multiple_of(4 * 32) as u32;
    let total_regs: u32 = 65536;

    let out = total_regs / local_size;
    // GPRs are allocated in multiples of 8
    let out = prev_multiple_of(out, 8);
    min(out, 255)
}

pub fn max_warps_per_sm(gprs: u32) -> u32 {
    fn prev_multiple_of(x: u32, y: u32) -> u32 {
        (x / y) * y
    }

    // TODO: Take local_size and shared mem limit into account for compute
    let total_regs: u32 = 65536;
    // GPRs are allocated in multiples of 8
    let gprs = gprs.next_multiple_of(8);
    let max_warps = prev_multiple_of((total_regs / 32) / gprs, 4);
    min(max_warps, 48)
}

pub struct Shader<'a> {
    pub sm: &'a dyn ShaderModel,
    pub info: ShaderInfo,
    pub functions: Vec<Function>,
}

impl Shader<'_> {
    pub fn for_each_instr(&self, f: &mut impl FnMut(&Instr)) {
        for func in &self.functions {
            for b in &func.blocks {
                for i in &b.instrs {
                    f(i);
                }
            }
        }
    }

    pub fn map_instrs(
        &mut self,
        mut map: impl FnMut(Box<Instr>, &mut SSAValueAllocator) -> MappedInstrs,
    ) {
        for f in &mut self.functions {
            f.map_instrs(&mut map);
        }
    }

    /// Remove all annotations, presumably before encoding the shader.
    pub fn remove_annotations(&mut self) {
        self.map_instrs(|instr: Box<Instr>, _| -> MappedInstrs {
            if matches!(instr.op, Op::Annotate(_)) {
                MappedInstrs::None
            } else {
                MappedInstrs::One(instr)
            }
        })
    }

    pub fn gather_info(&mut self) {
        let mut num_instrs = 0;
        let mut uses_global_mem = false;
        let mut writes_global_mem = false;
        let mut uses_fp64 = false;

        self.for_each_instr(&mut |instr| {
            num_instrs += 1;

            if !uses_global_mem {
                uses_global_mem = instr.uses_global_mem();
            }

            if !writes_global_mem {
                writes_global_mem = instr.writes_global_mem();
            }

            if !uses_fp64 {
                uses_fp64 = instr.op.is_fp64();
            }
        });

        self.info.num_instrs = num_instrs;
        self.info.uses_global_mem = uses_global_mem;
        self.info.writes_global_mem = writes_global_mem;
        self.info.uses_fp64 = uses_fp64;

        self.info.max_warps_per_sm = max_warps_per_sm(
            self.info.num_gprs as u32 + self.sm.hw_reserved_gprs(),
        );
    }
}

impl fmt::Display for Shader<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for func in &self.functions {
            write!(f, "{}", func)?;
        }
        Ok(())
    }
}
