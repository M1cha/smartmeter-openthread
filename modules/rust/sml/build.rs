//! Generates code for parsing the types from the SML specification

#![feature(exit_status_error)]

use askama::Template as _;
use convert_case::{Case, Casing};
use lazy_static::lazy_static;
use std::borrow::Borrow as _;
use std::io::BufRead as _;
use std::io::Write as _;

/// turns any string into a valid rust identifier
fn str2ident(string: &str, case: Case) -> String {
    lazy_static! {
        static ref RE_SEPARATE: regex::Regex = regex::Regex::new(r"[\.-]+").unwrap();
    }

    let string = RE_SEPARATE.replace(string, "_");

    let first = string.as_ref().chars().next().unwrap();
    let string = if first.is_numeric() {
        format!("N_{}", string)
    } else {
        string.to_string()
    };

    string.to_case(case)
}

fn is_primitive_rust(name: &str) -> bool {
    matches!(
        name,
        "u8" | "u16" | "u32" | "u64" | "i8" | "i16" | "i32" | "i64" | "bool"
    )
}

/// return a string with code that calls the sml decoding function and returns it's value
fn smlfn(ty: &str, optional: bool) -> String {
    let nextfn = match ty {
        "u8" | "u16" | "u32" | "u64" => "next_unsigned",
        "i8" | "i16" | "i32" | "i64" => "next_integer",
        "bool" => "next_boolean",
        _ => unimplemented!(),
    };
    let intofn = match ty {
        "u8" | "u16" | "u32" | "u64" | "i8" | "i16" | "i32" | "i64" => format!("into_{}", ty),
        "bool" => "into_bool".to_string(),
        _ => unimplemented!(),
    };

    if optional {
        format!(
            "{{
            match self.list.{}_opt().await? {{
                Some(v) => Some(v.{}().await?),
                None => None,
            }}
        }}",
            nextfn, intofn
        )
    } else {
        format!("self.list.{}().await?.{}().await?", nextfn, intofn)
    }
}

fn complexname(typename: &str, fieldname: &str) -> String {
    format!("{}{}Complex", typename, fieldname.to_case(Case::Pascal))
}

fn enum_generics(has_complexs: bool) -> &'static str {
    if has_complexs {
        "<'a, R>"
    } else {
        ""
    }
}

fn type2opt(name: &str, optional: bool) -> String {
    if optional {
        format!("Option<{}>", name)
    } else {
        name.to_string()
    }
}

#[derive(Debug, Default, serde::Serialize)]
struct Variant {
    ty: String,
    value: u64,
}

impl AsRef<str> for Variant {
    fn as_ref(&self) -> &str {
        &self.ty
    }
}

#[derive(Debug, Default, serde::Serialize)]
struct Choice {
    variants: std::collections::HashMap<String, Variant>,
}

#[derive(Debug, Default, serde::Serialize)]
struct ImplicitChoice {
    /// name, type
    types: std::collections::HashMap<String, String>,
}

#[derive(Debug, Default, serde::Serialize)]
struct Field {
    name: String,
    ty: String,
    optional: bool,
}

impl Field {
    pub fn name(&self) -> String {
        str2ident(&self.name, Case::Snake)
    }
}

#[derive(Debug, Default, serde::Serialize)]
struct Sequence {
    fields: Vec<Field>,
}

impl Sequence {
    fn fieldstruct_ident(&self, name: &str, id: usize) -> String {
        if id == 0 {
            name.to_string()
        } else {
            str2ident(&format!("{}_{}", name, self.fields[id].name), Case::Pascal)
        }
    }
}

#[derive(Debug, Default, serde::Serialize)]
struct SequenceOf {
    /// name, type
    types: std::collections::HashMap<String, String>,
}

#[derive(Debug, serde::Serialize)]
enum Type {
    Choice(Choice),
    ImplicitChoice(ImplicitChoice),
    Sequence(Sequence),
    SequenceOf(SequenceOf),
}

#[derive(askama::Template)]
#[template(path = "messages.rs", escape = "none")]
struct Template<'a> {
    types: &'a std::collections::HashMap<String, Type>,
    typedefs: &'a std::collections::HashMap<String, String>,
}

impl<'a> Template<'a> {
    pub fn type2rust(&self, name: &str) -> String {
        let name = self.typedefs.get(name).map(|s| s.as_str()).unwrap_or(name);
        let name = name.trim_start_matches("SML_");

        match name {
            "Unsigned8" => "u8",
            "Unsigned16" => "u16",
            "Unsigned32" => "u32",
            "Unsigned64" => "u64",
            "Integer8" => "i8",
            "Integer16" => "i16",
            "Integer32" => "i32",
            "Integer64" => "i64",
            "Boolean" => "bool",
            "Octet String" => "crate::tlv::String",
            other => return str2ident(other, Case::Pascal),
        }
        .to_string()
    }

    pub fn has_complexs<I: std::iter::Iterator<Item = Item>, Item: AsRef<str>>(
        &self,
        mut iter: I,
    ) -> bool {
        iter.any(|ty| !is_primitive_rust(&self.type2rust(ty.as_ref())))
    }

    pub fn type_to_tlvtypes(
        &self,
        name: &str,
    ) -> Vec<(&'static str, Option<std::ops::Range<usize>>)> {
        let name = self.typedefs.get(name).map(|s| s.as_str()).unwrap_or(name);

        if name == "Unsigned8" {
            vec![("Unsigned", Some(1..2))]
        } else if name == "Unsigned16" {
            vec![("Unsigned", Some(2..3))]
        } else if name == "Unsigned32" {
            vec![("Unsigned", Some(3..5))]
        } else if name == "Unsigned64" {
            vec![("Unsigned", Some(5..9))]
        } else if name == "Integer8" {
            vec![("Integer", Some(1..2))]
        } else if name == "Integer16" {
            vec![("Integer", Some(2..3))]
        } else if name == "Integer32" {
            vec![("Integer", Some(3..5))]
        } else if name == "Integer64" {
            vec![("Integer", Some(5..9))]
        } else if name == "Octet String" {
            vec![("String", None)]
        } else if name == "Boolean" {
            vec![("Boolean", None)]
        } else if let Some(parsed_ty) = self.types.get(name) {
            match parsed_ty {
                Type::Sequence(_) | Type::SequenceOf(_) | Type::Choice(_) => {
                    vec![("List", None)]
                }
                Type::ImplicitChoice(c) => {
                    let mut ret = Vec::new();

                    for variantty in c.types.values() {
                        ret.append(&mut self.type_to_tlvtypes(variantty));
                    }

                    ret
                }
            }
        } else {
            panic!("can't find tlvtype for {}", name)
        }
    }
}

fn is_cosem(name: &str) -> bool {
    lazy_static! {
        static ref RE: regex::Regex = regex::Regex::new(r"^SML_[a-zA-Z]*Cosem.*$").unwrap();
    }

    RE.is_match(name)
}

fn main() {
    env_logger::init();

    lazy_static! {
        static ref RE_START: regex::Regex = regex::Regex::new(r"([a-zA-Z0-9_\.]+)\s*::=\s*([a-zA-Z0-9_\. ]+)").unwrap();
        static ref RE_OPEN: regex::Regex = regex::Regex::new(r"^\s*\{\s*$").unwrap();
        static ref RE_CLOSE: regex::Regex = regex::Regex::new(r"^\s*\}\s*$").unwrap();
        static ref RE_CLOSE_WITH_COMMENT: regex::Regex = regex::Regex::new(r"^\s*\}\s+.*\)$").unwrap();
        static ref RE_FIELD: regex::Regex = regex::Regex::new(r"^\s*([a-zA-Z0-9_\-\.]+)\s+(\[0x([0-9a-fA-F]+)\]\s+)?([a-zA-Z0-9_\.\? ]+)\s*,?\s*(\(.*)?$").unwrap();
    }

    let doc_pdf_path = std::path::Path::new("specification/TR-03109-1_Anlage_Feinspezifikation_Drahtgebundene_LMN-Schnittstelle_Teilb.pdf");
    let out_path = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let doc_txt_path = out_path.join("spec-sml2pdf.txt");

    println!("cargo:rerun-if-changed={}", doc_pdf_path.to_str().unwrap());
    println!("cargo:rerun-if-changed=templates");

    std::process::Command::new("pdftotext")
        .arg("-layout")
        .arg(&doc_pdf_path)
        .arg(&doc_txt_path)
        .spawn()
        .expect("failed to spawn pdf2text")
        .wait()
        .expect("failed to wait for pdftotext")
        .exit_ok()
        .expect("pdftotext failed");

    let f = std::fs::File::open(&doc_txt_path).expect("can't open file");
    let mut lines = std::io::BufReader::new(f).lines().map(|l| l.unwrap());

    let mut types = std::collections::HashMap::<String, Type>::new();
    let mut typedefs = std::collections::HashMap::<String, String>::new();

    while let Some(line_start) = lines.next() {
        let (name, ty) = match RE_START.captures(&line_start) {
            None => continue,
            Some(caps) => (
                caps.get(1).unwrap().as_str().trim(),
                caps.get(2).unwrap().as_str().trim(),
            ),
        };

        let line = lines.next().unwrap();
        if !RE_OPEN.is_match(&line) {
            if name != "EndOfSmlMsg" {
                typedefs.insert(name.to_string(), ty.to_string());
            }
            continue;
        }

        log::trace!("MATCH: {}", line_start);

        let mut parsed_type = match ty {
            "CHOICE" => Type::Choice(Choice::default()),
            "IMPLICIT CHOICE" => Type::ImplicitChoice(ImplicitChoice::default()),
            "SEQUENCE" => Type::Sequence(Sequence::default()),
            "SEQUENCE OF" => Type::SequenceOf(SequenceOf::default()),
            other => panic!("unsupported type: {}", other),
        };

        'mainloop: loop {
            let line = lines.next().unwrap();
            if RE_CLOSE.is_match(&line) {
                break;
            }
            if line.is_empty() || line.trim() == "alle Datentyp aus GreenBook Seite 210 übernehmen!"
            {
                continue;
            }
            log::trace!("LINE: {}", line);

            let caps = RE_FIELD.captures(&line).unwrap();

            let name = caps.get(1).unwrap().as_str().trim();
            let value = caps.get(3);
            let (ty, optional) = {
                let s: Vec<_> = caps.get(4).unwrap().as_str().trim().split(' ').collect();

                if s.len() > 1 && s.last().unwrap() == &"OPTIONAL" {
                    (s[0..s.len() - 1].join(" ").trim().to_string(), true)
                } else {
                    (s.join(" "), false)
                }
            };
            let ty = match ty.as_ref() {
                "SML_Value10" => "SML_Value".to_string(),
                "Octet String9" => "Octet String".to_string(),
                "boolean" => "Boolean".to_string(),
                _ => ty,
            };
            let comment = caps.get(5).map(|v| v.as_str());

            if let Some(comment) = comment {
                if !comment.ends_with(')') {
                    loop {
                        let line = lines.next().unwrap();
                        if line.ends_with(')') {
                            if RE_CLOSE_WITH_COMMENT.is_match(&line) {
                                break 'mainloop;
                            } else {
                                break;
                            }
                        }
                    }
                }
            }

            match &mut parsed_type {
                Type::Sequence(seq) => {
                    if value.is_some() {
                        panic!("sequence fields can't have values");
                    }
                    seq.fields.push(Field {
                        name: name.to_string(),
                        ty,
                        optional,
                    });
                }
                Type::Choice(c) => {
                    if optional {
                        panic!("choice variants can't be optional");
                    }

                    let value: u64 = u64::from_str_radix(value.unwrap().as_str(), 16).unwrap();

                    if name != "SetProcParameterResponse" && !is_cosem(&ty) {
                        c.variants.insert(name.to_string(), Variant { ty, value });
                    }
                }
                Type::ImplicitChoice(c) => {
                    if value.is_some() {
                        panic!("implicit choices can't have values");
                    }
                    if optional {
                        panic!("implicit choice variants can't be optional");
                    }
                    c.types.insert(name.to_string(), ty);
                }
                Type::SequenceOf(seq) => {
                    if value.is_some() {
                        panic!("sequence-ofs can't have values");
                    }
                    if optional {
                        panic!("sequence-ofs can't be optional");
                    }
                    seq.types.insert(name.to_string(), ty);
                }
            }
        }

        if name == "..."
            || name == "Boolean"
            || name.starts_with("Unsigned")
            || name.starts_with("Integer")
            // this one has an incomplete specification
            || is_cosem(name)
        {
            continue;
        }

        types.insert(name.to_string(), parsed_type);
    }

    // validate
    for (name, parsed_type) in &types {
        match parsed_type {
            Type::Sequence(_) => {}
            Type::Choice(_) => {}
            Type::ImplicitChoice(c) => {
                let count = c
                    .types
                    .values()
                    .filter(|&variantty| types.get(variantty).is_some())
                    .count();
                if count > 1 {
                    panic!("choice {} has {} list types", name, count);
                }

                for variantty in c.types.values() {
                    let count = c
                        .types
                        .values()
                        .filter(|&variantty2| variantty2 == variantty)
                        .count();
                    if count > 1 {
                        panic!(
                            "choice {} has {} variants of type {}",
                            name, count, variantty
                        );
                    }
                }
            }
            Type::SequenceOf(seq) => {
                let count = seq
                    .types
                    .values()
                    .filter(|&variantty| types.get(variantty).is_some())
                    .count();
                if count > 1 {
                    panic!("sequenceof {} has {} list types", name, count);
                }

                for variantty in seq.types.values() {
                    let count = seq
                        .types
                        .values()
                        .filter(|&variantty2| variantty2 == variantty)
                        .count();
                    if count > 1 {
                        panic!(
                            "sequenceof {} has {} variants of type {}",
                            name, count, variantty
                        );
                    }
                }
            }
        }
    }

    let template = Template {
        types: &types,
        typedefs: &typedefs,
    };
    let code = template.render().unwrap();

    let mut f = std::fs::File::create(out_path.join("messages.rs")).unwrap();
    f.write_all(code.as_bytes()).unwrap();
}
