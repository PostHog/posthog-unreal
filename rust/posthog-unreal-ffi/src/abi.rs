//! Allocation, bounded JSON and panic containment shared by the ABI.
use std::panic::{catch_unwind, AssertUnwindSafe};

use serde::de::DeserializeOwned;
use serde::Serialize;

pub const ABI_VERSION: u32 = 3;
pub const MAX_INPUT: usize = 1024 * 1024;
pub const MAX_OUTPUT: usize = 4 * 1024 * 1024;
pub const MAX_HOOK: usize = 32 * 1024 * 1024;

#[repr(i32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum Status {
    Ok = 0,
    Accepted = 1,
    InvalidInput = 2,
    Closed = 3,
    InternalError = 5,
    UnsupportedAbi = 6,
    Disabled = 7,
    Full = 8,
    OutputTooLarge = 9,
    OptedOut = 10,
    Dropped = 11,
    Reentrant = 12,
    Deferred = 13,
    Inactive = 14,
    WrongRecorder = 15,
    Stale = 16,
}
pub type Result<T> = std::result::Result<T, Status>;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Bytes {
    pub data: *const u8,
    pub len: usize,
}
impl Bytes {
    pub fn from_slice(bytes: &[u8]) -> Self {
        Self {
            data: bytes.as_ptr(),
            len: bytes.len(),
        }
    }
    /// # Safety
    /// Caller guarantees readable memory for the supplied length and returned borrow.
    pub unsafe fn slice<'a>(self, limit: usize) -> Result<&'a [u8]> {
        if self.data.is_null()
            || self.len == 0
            || self.len > limit
            || self.len > isize::MAX as usize
        {
            return Err(Status::InvalidInput);
        }
        Ok(unsafe { std::slice::from_raw_parts(self.data, self.len) })
    }
}
#[repr(C)]
pub struct Buffer {
    pub data: *mut u8,
    pub len: usize,
}
impl Buffer {
    pub fn empty() -> Self {
        Self {
            data: std::ptr::null_mut(),
            len: 0,
        }
    }
    pub fn new(bytes: Vec<u8>) -> Self {
        let boxed = bytes.into_boxed_slice();
        let len = boxed.len();
        Self {
            data: Box::into_raw(boxed) as *mut u8,
            len,
        }
    }
    /// # Safety
    /// Must be an unchanged allocation returned by this ABI, exactly once.
    pub unsafe fn free(self) {
        if !self.data.is_null() {
            unsafe {
                drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
                    self.data, self.len,
                )))
            };
        }
    }
}

pub fn contain(f: impl FnOnce() -> Result<Status>) -> Status {
    catch_unwind(AssertUnwindSafe(f))
        .unwrap_or(Err(Status::InternalError))
        .unwrap_or_else(|e| e)
}
pub unsafe fn parse<T: DeserializeOwned>(input: Bytes) -> Result<T> {
    let bytes = unsafe { input.slice(MAX_INPUT)? };
    if bytes.iter().copied().find(|b| !b.is_ascii_whitespace()) != Some(b'{') {
        return Err(Status::InvalidInput);
    }
    serde_json::from_slice(bytes).map_err(|_| Status::InvalidInput)
}
pub fn encode(value: &impl Serialize, limit: usize) -> Result<Vec<u8>> {
    // Stop serialization at the boundary, rather than allocate an unbounded result.
    struct Bounded {
        bytes: Vec<u8>,
        limit: usize,
    }
    impl std::io::Write for Bounded {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            if bytes.len() > self.limit.saturating_sub(self.bytes.len()) {
                return Err(std::io::Error::other("ABI output exceeds limit"));
            }
            self.bytes.extend_from_slice(bytes);
            Ok(bytes.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }
    let mut writer = Bounded {
        bytes: Vec::new(),
        limit,
    };
    serde_json::to_writer(&mut writer, value).map_err(|_| Status::OutputTooLarge)?;
    Ok(writer.bytes)
}
pub unsafe fn clear(out: *mut Buffer) -> Result<()> {
    if out.is_null() {
        return Err(Status::InvalidInput);
    }
    unsafe { *out = Buffer::empty() };
    Ok(())
}
pub unsafe fn output(out: *mut Buffer, value: &impl Serialize) -> Result<()> {
    unsafe { *out = Buffer::new(encode(value, MAX_OUTPUT)?) };
    Ok(())
}

#[no_mangle]
pub extern "C" fn phu_abi_version() -> u32 {
    ABI_VERSION
}

/// # Safety
/// Input is readable, output writable/disjoint. Free the resulting buffer once.
#[no_mangle]
pub unsafe extern "C" fn phu_buffer_copy(input: Bytes, out: *mut Buffer) -> Status {
    contain(|| {
        unsafe {
            clear(out)?;
            *out = Buffer::new(input.slice(MAX_HOOK)?.to_vec());
        }
        Ok(Status::Ok)
    })
}
/// # Safety
/// Pass an unchanged ABI allocation exactly once, or {NULL,0}.
#[no_mangle]
pub unsafe extern "C" fn phu_buffer_free(value: Buffer) {
    let _ = catch_unwind(AssertUnwindSafe(|| unsafe { value.free() }));
}
