# How to successfully make changes to Ninja

We're very wary of changes that increase the complexity of Ninja (in particular,
new build file syntax or command-line flags) or increase the maintenance burden
of Ninja. Ninja is already successfully used by hundreds of developers for large
projects and it already achieves (most of) the goals we set out for it to do.
It's probably best to discuss new feature ideas on the
[mailing list](https://groups.google.com/forum/#!forum/ninja-build) or in an
issue before creating a PR.

## Coding guidelines

Generally it's the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) with
a few additions:

* All source files should have the Google Inc. license header.
* Use `///` for [Doxygen](http://www.doxygen.nl/) (use `\a` to refer to
  arguments).
* It's not necessary to document each argument, especially when they're
  relatively self-evident (e.g. in
  `CanonicalizePath(string* path, string* err)`, the arguments are hopefully
  obvious).

If you're unsure about code formatting, please use
[clang-format](https://clang.llvm.org/docs/ClangFormat.html). However, please do
not format code that is not otherwise part of your contribution.
