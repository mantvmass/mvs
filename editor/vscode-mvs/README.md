# MVS for VS Code

Syntax highlighting for `.mvs` files, plus comment toggling, bracket matching,
and auto-closing pairs. VS Code highlights through TextMate grammars, not
tree-sitter, so this extension carries its own grammar
(`syntaxes/mvs.tmLanguage.json`) covering the same construct set as
[../tree-sitter-mvs](../tree-sitter-mvs): declarations, traits, enums + match,
generics, attributes, `asm`, all the literal forms, and the `{}` placeholders
inside io.out format strings.

## Install

No marketplace needed. Copy this folder into your extensions directory and
reload VS Code:

```powershell
# Windows
Copy-Item -Recurse editor\vscode-mvs "$env:USERPROFILE\.vscode\extensions\mantvmass.mvs-0.1.0"
```

```sh
# Linux / macOS
cp -r editor/vscode-mvs ~/.vscode/extensions/mantvmass.mvs-0.1.0
```

Then run "Developer: Reload Window" (or restart VS Code) and open any `.mvs`
file.

## Developing it

The grammar is tested by tokenizing `tests/sample.mvs` and comparing against
the committed snapshot:

```sh
npm install
npm test              # verify tokenization against tests/sample.mvs.snap
npx vscode-tmgrammar-snap --updateSnapshot -g syntaxes/mvs.tmLanguage.json "tests/*.mvs"
```

When the language grows new syntax, extend the grammar, extend the sample,
regenerate the snapshot, and read the diff. TextMate is regex per line, so
keep expectations modest: the tree-sitter grammar is the precise one, this is
the pragmatic one.
