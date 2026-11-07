# Earthshine measurements

Host programs that check what the ear cares about. Build each with the SDK's
headers and Earthshine's folder on the include path, for example:

```sh
g++ -O2 -std=c++17 -I../open-xy/sdk/include -Iearthshine tools/measure/earthshine/tailtune.cpp -o tailtune
```

| Program | Measures |
| --- | --- |
| `tailtune.cpp` | The pitch low notes' tails ring at, in cents (`-DMOD=`): the tank's resonance density |
| `ned.cpp` | Echo density of the impulse response, early and late (1 is a smooth wash) |
| `clarity.cpp` | How cleanly the sub holds pitch under low notes |
| `chordclar.cpp` | The same under a low chord |
| `swell90.cpp` | When the shimmer reaches 50% and 90% for each Swell |
| `ringtest.cpp` | Block-rate ring on the sub for each Swell |
| `zip.cpp` | Block-rate buzz with Mod (`-DMOD=`, `-DOFF=` for a control band) |
