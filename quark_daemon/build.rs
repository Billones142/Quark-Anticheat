use std::env;
use std::path::PathBuf;

use libbpf_cargo::SkeletonBuilder;

const BPF_SRC: &str = "src/bpf/quark.bpf.c";

fn main() {
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR must be set by cargo"))
        .join("quark.skel.rs");

    // src/bpf/vmlinux.h is generated from the build host's kernel BTF (top-level
    // Makefile's `vmlinux` target), not committed. CO-RE relocations make the
    // resulting object portable to other kernels anyway.
    SkeletonBuilder::new()
        .source(BPF_SRC)
        .clang_args(["-I", "src/bpf", "-Wall", "-Werror", "-Wno-missing-declarations"])
        .build_and_generate(&out)
        .expect(
            "failed to build BPF skeleton (is src/bpf/vmlinux.h generated? run `make vmlinux`)",
        );

    println!("cargo:rerun-if-changed={BPF_SRC}");
    println!("cargo:rerun-if-changed=src/bpf/quark_events.h");
    println!("cargo:rerun-if-changed=src/bpf/vmlinux.h");
}
