<!-- DISCLAIMER: maintainer adds this -->

# tinyfota

Signed, power-cut-safe delta firmware updates for chips that cannot hold two
images. The patch is applied in place, from inside unmodified MCUboot, and
delivered from a phone app, a browser or a cable.

Extracted from ultrawidelock@d6e23761.

Status: scaffold. The sections below are filled in as the import lands.

## Build and test

```
make test     # host unit tests
make asan     # the same tests under AddressSanitizer and UndefinedBehaviorSanitizer
make seams    # the symbols the core expects a port to provide
```

On a machine where `cc` or `nm` is not the tool you want, pass both:
`make test CC=/usr/bin/clang NM="xcrun nm"`.

## License

ISC, see `LICENSE`. Third-party code is listed in `THIRD_PARTY.md`.
