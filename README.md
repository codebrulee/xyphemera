# Xyphemera

Open-XY machines for the OP-XY: effects about light, time and memory. Each lives
in its own folder, with its own definition, tests, README and catalog entry.

| Machine | Kind | What it is |
| --- | --- | --- |
| [earthshine](earthshine/README.md) | send effect | exploring the musical and emotional impact of the light touching the earth |

## Develop

Keep the [Open-XY SDK](https://github.com/etcetc000/open-xy) beside this
repository as `../open-xy`, then test a machine:

```sh
python ../open-xy/sdk.py test --machine earthshine
```

`tools/` holds what the machines share: an instruction counter on the GNU
Blackfin simulator, an A/B renderer and CPU probe effects
([tools/README.md](tools/README.md)), and each machine's own measurements in
`tools/measure/<machine>/` (outside the machine's folder, which the SDK checks
as machine code).

## Releases

Each machine is tagged on its own, as `<id>-v<version>` (for example
`earthshine-v1.0.0`); its catalog entry pins that tag, its commit and its
folder, so releasing one never changes another.

Released under the MIT licence ([LICENSE](LICENSE)).
