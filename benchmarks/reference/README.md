# Frozen scaling reference

`AreaResize-before-axis.cpp` is the unmodified production source from commit
`1ba4391d172bf5fc2fa057cae3e97142abeab52d`, before horizontal weight reuse.
SHA-256: `bd642e69f5b9441b6d74c74264970e8e9a02c9780c1a254365a1d85c76f16e4b`.

This snapshot is compiled only by the opt-in Docker comparison stage, against the
same headers, dependencies, flags and benchmark driver as the current source.
It is not linked into the server. Keep it frozen; it is historical measurement
evidence, not a second implementation to maintain. If the surrounding interfaces
change incompatibly, retire or explicitly rebase the experiment rather than
silently altering its baseline.
