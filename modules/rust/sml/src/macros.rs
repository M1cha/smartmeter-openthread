macro_rules! bitvalues {
    (
        $(#[$outer:meta])*
        $vis:vis enum $name:ident : $ty:ident {
            $(
                $variant:ident = $value:expr,
            )*
        }
    ) => {
        $(#[$outer])*
        $vis enum $name {
            $( $variant, )*
            Other($ty),
        }

        impl core::convert::From<$ty> for $name {
            fn from(byte: $ty) -> Self {
                match byte {
                    $( $value => Self::$variant, )*
                    other => Self::Other(other),
                }
            }
        }

        impl core::convert::From<$name> for $ty {
            fn from(v: $name) -> $ty {
                match v {
                    $( $name::$variant => $value, )*
                    $name::Other(other) => other,
                }
            }
        }
    };
}

pub(crate) use bitvalues;
