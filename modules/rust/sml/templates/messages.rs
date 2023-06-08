// TODO: implement optionals for non-primitives
// TODO: implement sequenceofs with multiple value-types

{% macro render_sequence_fieldfn(typename, seq, id, num_skip) %}
    {% let field = seq.fields[id] %}
    {% let fieldty = self.type2rust(field.ty) %}
    {% let fieldty_opt = crate::type2opt(fieldty, field.optional.clone()) %}
    {% let fieldname = field.name() %}
    {% let complexname = crate::complexname(typename, fieldname) %}

    {% if crate::is_primitive_rust(fieldty) %}
        {% if seq.fields.get(id.clone() + 1).is_some() %}
            {% let nextty = seq.fieldstruct_ident(typename, id.clone() + 1) %}

            pub async fn {{fieldname}} (mut self) -> Result<({{nextty}} <'a, R>, {{fieldty_opt}}), crate::Error> {
                {% if num_skip > 0 %}
                    self.list.skip({{num_skip}}).await?;
                {% endif %}

                let val = {{crate::smlfn(fieldty, field.optional.clone())}};
                Ok(({{nextty}} { list: self.list }, val))
            }
        {% else %}
            // this is the last field, so we don't return the next reader
            pub async fn {{fieldname}} (mut self) -> Result<{{fieldty_opt}}, crate::Error> {
                {% if num_skip > 0 %}
                    self.list.skip({{num_skip}}).await?;
                {% endif %}

                Ok({{crate::smlfn(fieldty, field.optional.clone())}})
            }
        {% endif %}
    {% else if fieldty == "EndOfSmlMsg" %}
        pub async fn {{fieldname}} (mut self) -> Result<(), crate::Error> {
            {% if num_skip > 0 %}
                    self.list.skip({{num_skip}}).await?;
            {% endif %}

            self.list.next_end().await
        }
    {% else %}
        {% if seq.fields.get(id.clone() + 1).is_some() %}
            {% let nextty = seq.fieldstruct_ident(typename, id.clone() + 1) %}

            pub async fn {{fieldname}}<'x: 'a>(mut self) -> Result<{{complexname}}<'a, R, {{nextty}}<'a, R>>, crate::Error> {
                {% if num_skip > 0 %}
                    self.list.skip({{num_skip}}).await?;
                {% endif %}

                Ok({{complexname}}::new(self.list))
            }
            {% else %}
            pub async fn {{fieldname}}(mut self) -> Result<{{complexname}}<'a, R, ()>, crate::Error> {
                {% if num_skip > 0 %}
                    self.list.skip({{num_skip}}).await?;
                {% endif %}

                Ok({{complexname}}::new(self.list))
            }
        {% endif %}
    {% endif %}
{% endmacro %}

{% macro render_sequence(typename, seq) %}
    {% for (id, field) in seq.fields.iter().enumerate() %}
        {% let structname = seq.fieldstruct_ident(typename, id.clone()) %}
        {% let fieldty = self.type2rust(field.ty) %}
        {% let fieldname = field.name() %}

        pub struct {{structname}} <'a, R> {
            pub(crate) list: crate::tlv::List<'a, R>,
        }

        impl<'a, R> FromTlvList<'a, R> for {{structname}}<'a, R> {
            fn from_tlv_list(list: crate::tlv::List<'a, R>) -> Self {
                Self {list}
            }
        }

        impl<'a, R: 'a> FromTlvItem<'a, R> for {{structname}}<'a, R> {
            async fn from_tlv_item(item: crate::tlv::Item<'a, R>) -> Result<Self, crate::Error> {
                match item {
                    crate::tlv::Item::List(list) => Ok(Self {list}),
                    _ => Err(crate::Error::UnexpectedValue),
                }
            }
        }

        {% if id == 0 %}
            impl<'a: 'b, 'b, R: io::AsyncRead + Unpin + 'a> ParseField<'a, 'b, R> for {{structname}}<'b, R> {
                async fn parse_field<'l: 'b>(list: &'l mut crate::tlv::List<'a, R>) -> Result<Self, crate::Error> {
                    Ok(Self {
                        list: list.next_list().await?,
                    })
                }
            }
        {% endif %}

        {% if !crate::is_primitive_rust(fieldty) && fieldty != "EndOfSmlMsg" %}
            {% let complexname = crate::complexname(typename, fieldname) %}

            pub struct {{complexname}}<'a, Reader, NextTy> {
                list: crate::tlv::List<'a, Reader>,
                parsed: bool,
                pd: core::marker::PhantomData<NextTy>,
            }

            impl<'a, Reader: io::AsyncRead + Unpin, NextTy> {{complexname}}<'a, Reader, NextTy>
            where
                NextTy: FromTlvList<'a, Reader>,
            {
                pub fn new(list: crate::tlv::List<'a, Reader>) -> Self {
                    Self {
                        list,
                        parsed: false,
                        pd: core::marker::PhantomData,
                    }
                }

                pub async fn parse<'s>(&'s mut self) -> Result<{{fieldty}}<'s, Reader>, crate::Error>
                {
                    if self.parsed {
                        return Err(crate::Error::CantParseTwice);
                    }
                    self.parsed = true;

                    {{fieldty}}::parse_field(&mut self.list).await
                }

                pub async fn finish(mut self) -> Result<NextTy, crate::Error> {
                    if !self.parsed {
                        {{fieldty}}::parse_field(&mut self.list).await?;
                    }

                    Ok(NextTy::from_tlv_list(self.list))
                }
            }
        {% endif %}

        #[allow(unused_mut)]
        impl<'a, R: io::AsyncRead + Unpin> {{structname}} <'a, R> {
            {% for id2 in id..seq.fields.len() %}
                {% let num_skip = id2 - id %}
                {% call render_sequence_fieldfn(typename, seq, id2, num_skip) %}
            {% endfor %}
        }
        {% endfor %}
{% endmacro %}

{% macro render_sequence_of_single(typename, valuetype_raw) %}
    {% let structname = crate::str2ident(typename, Case::Pascal) %}
    {% let valuetype = self.type2rust(valuetype_raw) %}

    pub struct {{structname}} <'a, R> {
        pub(crate) list: crate::tlv::List<'a, R>,
    }

    impl<'a: 'b, 'b, R: io::AsyncRead + Unpin + 'a> ParseField<'a, 'b, R> for {{structname}}<'b, R> {
        async fn parse_field<'l: 'b>(list: &'l mut crate::tlv::List<'a, R>) -> Result<Self, crate::Error> {
            Ok(Self {
                list: list.next_list().await?,
            })
        }
    }

    impl<'a, R: io::AsyncRead + Unpin> {{structname}} <'a, R> {
        pub async fn next<'s>(&'s mut self) -> Result<Option<{{valuetype}}<'s, R>>, crate::Error> {
            if self.list.len() == 0 {
                return Ok(None);
            }

            Ok(Some(
            {% if crate::is_primitive_rust(valuetype.as_str()) %}
                Ok({{crate::smlfn(valuetype.as_str(), false)}})
            {% else %}
                {{valuetype}}::parse_field(&mut self.list).await?
            {% endif %}
            ))
        }
    }
{% endmacro %}

{% macro render_sequence_of_multi(typename, seq) %}
    {% let structname = crate::str2ident(typename, Case::Pascal) %}

    pub struct {{structname}} <'a, R> {
        _list: crate::tlv::List<'a, R>,
    }

    impl<'a: 'b, 'b, R: io::AsyncRead + Unpin + 'a> ParseField<'a, 'b, R> for {{structname}}<'a, R> {
        async fn parse_field<'l: 'b>(list: &'l mut crate::tlv::List<'a, R>) -> Result<Self, crate::Error> {
            log::error!("not implemented: {{structname}}");
            Err(crate::Error::UnexpectedValue)
        }
    }
{% endmacro %}

{% macro render_variant_enum(structname, variants) %}
    {% let has_complexs = self.has_complexs(variants.values()) %}
    {% let enum_generics = crate::enum_generics(has_complexs.clone()) %}

    pub enum {{structname}}Enum{{enum_generics}} {
        {% for (variantname, variant) in variants %}
            {% let variantname = crate::str2ident(variantname, Case::Pascal) %}
            {% let variantty = self.type2rust(variant.as_ref()) %}

            {% if crate::is_primitive_rust(variantty) %}
                {{variantname}}({{variantty}}),
            {% else %}
                {{variantname}}({{variantty}}<'a, R>),
            {% endif %}
        {% endfor %}
    }
{% endmacro %}

{% macro render_choice(typename, choice) %}
    {% let structname = crate::str2ident(typename, Case::Pascal) %}

    {% call render_variant_enum(structname, choice.variants.borrow()) %}

    pub struct {{structname}} <'r, R> {
        list: crate::tlv::List<'r, R>,
        parsed: bool,
    }

    impl<'r: 'b, 'b, R: io::AsyncRead + Unpin + 'r> ParseField<'r, 'b, R> for {{structname}}<'b, R> {
        async fn parse_field<'l: 'b>(list: &'l mut crate::tlv::List<'r, R>) -> Result<Self, crate::Error> {
            let list = list.next_list().await?;
            if list.len() != 2 {
                return Err(crate::Error::UnsupportedLen{len: list.len()});
            }

            Ok(Self {
                list,
                parsed: false,
            })
        }
    }

    impl<'a, R: 'a> FromTlvItem<'a, R> for {{structname}}<'a, R> {
        async fn from_tlv_item(item: crate::tlv::Item<'a, R>) -> Result<Self, crate::Error> {
            match item {
                crate::tlv::Item::List(list) => Ok(Self {
                    list,
                    parsed: false,
                }),
                _ => Err(crate::Error::UnexpectedValue),
            }
        }
    }

    impl<'a, R: io::AsyncRead + Unpin> {{structname}} <'a, R> {
        pub async fn read<'s>(&'s mut self) -> Result<{{structname}}Enum<'s, R>, crate::Error> {
            if self.parsed {
                return Err(crate::Error::CantParseTwice);
            }
            self.parsed = true;

            let tag = self.list.next_unsigned().await?.into_u32_relaxed().await?;

            Ok(match tag {
                {% for (variantname, variant) in choice.variants %}
                    {% let variantty = self.type2rust(variant.ty) %}
                    {% let variantname = crate::str2ident(variantname, Case::Pascal) %}

                    {{variant.value}} => {{structname}}Enum::{{variantname}}(
                        {% if crate::is_primitive_rust(variantty) %}
                            {{crate::smlfn(variantty, false)}}
                        {% else %}
                            {{variantty}}::parse_field(&mut self.list).await?
                        {% endif %}
                    ),
                {% endfor %}
                _ => return Err(crate::Error::UnsupportedTag{tag}),
            })
        }
    }
{% endmacro %}

{% macro render_implicit_choice(typename, choice) %}
    {% let structname = crate::str2ident(typename, Case::Pascal) %}

    {% call render_variant_enum(structname, choice.types.borrow()) %}
    {% let has_complexs = self.has_complexs(choice.types.values()) %}
    {% let enum_generics = crate::enum_generics(has_complexs.clone()) %}

    pub struct {{structname}} <'a, R> {
        item: crate::tlv::Item<'a, R>,
    }

    impl<'a: 'b, 'b, R: io::AsyncRead + Unpin + 'a> ParseField<'a, 'b, R> for {{structname}}<'b, R> {
        async fn parse_field<'l: 'b>(list: &'l mut crate::tlv::List<'a, R>) -> Result<Self, crate::Error> {
            Ok(Self {
                item: list.next_any().await?,
            })
        }
    }

    impl<'a, R: io::AsyncRead + Unpin + 'a> {{structname}} <'a, R> {
        pub async fn read(self) -> Result<{{structname}}Enum{{enum_generics}}, crate::Error> {
            match &self.item {
                {% for (variantname, variant) in choice.types.borrow() %}
                    {% let variantname = crate::str2ident(variantname, Case::Pascal) %}
                    {% let variantty = self.type2rust(variant.as_ref()) %}
                    {% let tlv_types = self.type_to_tlvtypes(variant.as_ref()) %}

                    {% for (tlv_type, range) in tlv_types %}
                        crate::tlv::Item::{{tlv_type}}(v)
                        {% if let Some(range) = range %}
                            if ({{range.start}}..{{range.end}}).contains(&v.len)
                        {% endif %}
                            => Ok({{structname}}Enum::{{variantname}}(
                            {{variantty}}::from_tlv_item(self.item).await?
                        )),
                    {% endfor %}
                {% endfor %}

                _ => Err(crate::Error::UnexpectedValue),
            }
        }
    }
{% endmacro %}

{% for (typename, ty) in types %}
    {% let typename = self.type2rust(typename) %}

    {% match ty %}
    {% when Type::Sequence with (seq) %}
        {% call render_sequence(typename, seq) %}
    {% when Type::SequenceOf with (seq) %}
        {% if seq.types.len() == 1 %}
            {% call render_sequence_of_single(typename, seq.types.values().next().unwrap().as_str()) %}
        {% else %}
            {% call render_sequence_of_multi(typename, seq) %}
        {% endif %}
    {% when Type::Choice with (choice) %}
        {% call render_choice(typename, choice) %}
    {% when Type::ImplicitChoice with (choice) %}
        {% call render_implicit_choice(typename, choice) %}
    {% endmatch %}
{% endfor %}
