# Branch model

Which branch to read, and why one of them is called `openke`. Written against the `openke`
branch at `95d0321e`, which is what NebulaOS 1.0.2 ships (`KERNEL_PIN` in the firmware manifest
at `3bdf3ff`, with `KERNEL_BRANCH=openke`) and is also the branch tip.


| Branch | What it is |
|---|---|
| `main` | `a98c2e1f2`, "initial release" - the **pristine vendor baseline**, forked from `Llixuma/ingenic-linux-kernel6.6-x2000-v1.0-20250221` |
| `openke` | The branch NebulaOS builds from: 133 commits on top of that baseline |

`openke` is the default branch, and the one to read. `main` exists so the vendor baseline stays
available for comparison - `git diff origin/main origin/openke` is the complete set of changes.

**A caution about that diff:** it reports around 13,000 files changed, which is not a measure of
NebulaOS's footprint. Most of it is the vendor tree reorganisation that came with the OpenKE
import. The NebulaOS-specific surface is small and listed below.


## The name

The branch is called `openke` because this repository's history runs through the
[OpenKE](https://github.com/coreflake1/guppyscreen) import, and the name was never changed when
the repository was renamed. **OpenKE is a separate project** that shares an author and some
history with NebulaOS; it is not the same thing, and its documentation does not describe
NebulaOS. Read nothing into the branch name beyond its history - see
[What NebulaOS changes](What-NebulaOS-Changes), whose first layer is exactly the work inherited
from it.

## Which one ships

The firmware repository pins an exact commit here and checks it out directly. `KERNEL_PIN` is
what ships; this branch's tip is not, when the two differ. At 1.0.2 they are the same commit,
`95d0321e`. See [Build and integration](Build-And-Integration).
