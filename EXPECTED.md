# The contract these fixtures assert

Every repository in this organization ships the same `.cli-flags.toml` and
asserts the same two results. A binding that disagrees with this file is broken.

## Defaults, no flags passed

argv: `["demo"]`

```
APP_ENV=development
COLOR=true
DEBUG=false
PORT=3000
```

## Overrides

argv: `["demo", "--port", "8181", "--debug=t", "--mode", "production"]`

```
APP_ENV=production
COLOR=true
DEBUG=true
PORT=8181
```

Note what is *not* happening here: `COLOR` was never passed, and it still
appears, because `parse` emits declared defaults alongside overrides. Short
forms (`-p 8181`), `=`-joined forms (`--debug=t`), space-separated forms
(`--port 8181`), and alias forms (`--listen-port 8181`, `--env production`) all
resolve to the same map.

The reference implementation of this contract is the CLI in the canonical
repository:

```console
$ flags2env --port 8181 --debug=t --mode production
{"PORT":"8181","DEBUG":"true","APP_ENV":"production","COLOR":"true"}
```
