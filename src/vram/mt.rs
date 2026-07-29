#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(unused)]
include!(concat!(env!("OUT_DIR"), "/mt_ffi.rs"));

use crate::{
    common::DataFormat::*,
    vram::inner::{DecodeCalls, EncodeCalls, InnerDecodeContext, InnerEncodeContext},
};
use std::os::raw::{c_int, c_void};

pub fn encode_calls() -> EncodeCalls {
    EncodeCalls {
        new: mt_new_encoder,
        encode: mt_encode,
        destroy: mt_destroy_encoder,
        test: mt_test_encode,
        set_bitrate: mt_set_bitrate,
        set_framerate: mt_set_framerate,
    }
}

pub fn possible_support_encoders() -> Vec<InnerEncodeContext> {
    if unsafe { mt_encode_driver_support() } != 0 {
        return vec![];
    }
    let dataFormats = vec![H264, H265];
    let mut v = vec![];
    for dataFormat in dataFormats.iter() {
        v.push(InnerEncodeContext {
            format: dataFormat.clone(),
        });
    }
    v
}

// Decode not yet supported by MT backend
unsafe extern "C" fn mt_new_decoder_stub(
    _device: *mut c_void,
    _luid: i64,
    _dataFormat: i32,
) -> *mut c_void {
    std::ptr::null_mut()
}

unsafe extern "C" fn mt_decode_stub(
    _decoder: *mut c_void,
    _data: *mut u8,
    _length: i32,
    _callback: crate::common::DecodeCallback,
    _obj: *mut c_void,
) -> c_int {
    -1
}

unsafe extern "C" fn mt_destroy_decoder_stub(_decoder: *mut c_void) -> c_int {
    0
}

unsafe extern "C" fn mt_test_decode_stub(
    _outLuids: *mut i64,
    _outVendors: *mut i32,
    _maxDescNum: i32,
    outDescNum: *mut i32,
    _dataFormat: i32,
    _data: *mut u8,
    _length: i32,
    _excludedLuids: *const i64,
    _excludeFormats: *const i32,
    _excludeCount: i32,
) -> c_int {
    *outDescNum = 0;
    -1
}

pub fn decode_calls() -> DecodeCalls {
    DecodeCalls {
        new: mt_new_decoder_stub,
        decode: mt_decode_stub,
        destroy: mt_destroy_decoder_stub,
        test: mt_test_decode_stub,
    }
}

pub fn possible_support_decoders() -> Vec<InnerDecodeContext> {
    vec![]
}
