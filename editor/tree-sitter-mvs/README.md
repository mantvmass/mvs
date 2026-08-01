# tree-sitter-mvs

A [tree-sitter](https://tree-sitter.github.io) grammar for MVS, for syntax
highlighting in editors that speak tree-sitter (Neovim, Helix, Zed, Emacs).

This is the editor-side parser only. The compiler's parser in `src/parser.c`
stays the single source of truth for the language; this grammar follows it,
and is checked by parsing every `.mvs` file in the repository (examples, std,
core, unit tests, diff tests) with zero errors.

## Using it

Neovim (nvim-treesitter):

```lua
local parsers = require("nvim-treesitter.parsers").get_parser_configs()
parsers.mvs = {
  install_info = {
    url = "https://github.com/mantvmass/mvs",
    location = "editor/tree-sitter-mvs",
    files = { "src/parser.c" },
  },
  filetype = "mvs",
}
vim.filetype.add({ extension = { mvs = "mvs" } })
```

Then `:TSInstall mvs`, and copy `queries/highlights.scm` to
`queries/mvs/highlights.scm` in your runtime path.

Helix (`languages.toml`):

```toml
[[language]]
name = "mvs"
scope = "source.mvs"
file-types = ["mvs"]

[[grammar]]
name = "mvs"
source = { git = "https://github.com/mantvmass/mvs", subpath = "editor/tree-sitter-mvs" }
```

Then `hx --grammar fetch && hx --grammar build`, and copy the queries into
`runtime/queries/mvs/`.

## Developing it

The generated `src/parser.c` is committed, so editors never need node. To
change the grammar you do need node:

```sh
npm install               # pulls the tree-sitter CLI
npx tree-sitter generate  # grammar.js -> src/parser.c
npx tree-sitter test      # the corpus in test/corpus/
```

The real check is parsing the repository: from this directory,

```sh
npx tree-sitter parse ../../examples/**/*.mvs ../../std/*.mvs ../../core/*.mvs --quiet --stat
```

must stay at 100%. When the language grows new syntax, extend `grammar.js`,
regenerate, and add a corpus case.
