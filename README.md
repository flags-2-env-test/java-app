# java-app

A Java 21 / JNI consumer of
**[`flags-2-env`](https://github.com/ORESoftware/flags-2-env)**.

This is a test fixture in the
[`flags-2-env-test`](https://github.com/flags-2-env-test) organization, one of
twelve. Each one ships a byte-identical [`.cli-flags.toml`](.cli-flags.toml) and
asserts a byte-identical parse result, so a binding that drifts from the C core
fails loudly and in isolation.

**The library source is not here.** It lives at
[github.com/ORESoftware/flags-2-env](https://github.com/ORESoftware/flags-2-env)
and is vendored into this repo, unmodified, at
`.vendor/.zed/oresoftware/flags-2-env`.

## Run it

```bash
git clone --recurse-submodules https://github.com/flags-2-env-test/java-app
cd java-app
docker build -t f2e-fixture-java .
docker run --rm f2e-fixture-java
```

The container is the test. It exits non-zero if any assertion in
[`EXPECTED.md`](EXPECTED.md) fails.

## How the dependency is declared

[`.zpkg.toml`](.zpkg.toml) is the declaration of record:

```toml
[install]
dir = ".vendor/.zed"
adapter = "java"

[dependencies]
"oresoftware/flags-2-env" = "^0.2.0"
```

`zed install` materializes that dependency at
`.vendor/.zed/oresoftware/flags-2-env`. A **git submodule is pinned at that
exact path** as the offline stand-in, which is what lets `docker build .`
succeed with no registry reachable and what lets CI run without bootstrapping
the `zed` CLI first. The two mechanisms write to the same location on purpose —
they are interchangeable, and pointing them at different directories is the
mistake this layout is designed to avoid.

The org segment in the dependency key is `oresoftware`, lowercase. That is the
`org` field of the publisher's own `.zpkg.toml` upstream, which is *not* the
same string as the GitHub owner (`ORESoftware`) and not this org.

## What it asserts

See [EXPECTED.md](EXPECTED.md) for the full contract. In short, given

```
demo --port 8181 --debug=t --mode production
```

every runtime must produce

```
APP_ENV=production
COLOR=true
DEBUG=true
PORT=8181
```

`COLOR` was never passed and still appears: `parse` emits declared defaults
alongside overrides. The fixture also asserts that the short form (`-p 8181`),
the alias form (`--env production`), and the `=`-joined form (`--debug=t`) all
collapse to that same map, and that `--no-color` negates a `bool` whose default
is true.

## License

MIT. The vendored library is MIT as well; see its own `LICENSE`.
