#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(unused)]
include!(concat!(env!("OUT_DIR"), "/mt_ffi.rs"));

use crate::{
    common::DataFormat::H264,
    vram::inner::{EncodeCalls, InnerEncodeContext},
};

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
    vec![InnerEncodeContext { format: H264 }]
}
