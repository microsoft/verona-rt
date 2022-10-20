## Project Verona
It is a research programming language to explore the concept of
concurrent ownership.  We are providing a new concurrency model that seamlessly
integrates ownership.

This research project is in its early stage and is open sourced to facilitate 
academic collaborations.  We are keen to engage in research collaborations on
this project, please do reach out to discuss this.

The project is not ready to be used outside of research.


![Github Repo Size](https://img.shields.io/github/repo-size/microsoft/verona-rt?style=for-the-badge&color=aqua)

# Status

This project is in its very early stage, parts of the type checker are still to be
implemented, and there are very few language features implemented yet. This will
change, but it would take time.

## Nightly Build

System \ Build Type | Release | Debug | ASAN+UBSAN+LSAN
--------|---------|-------|--------
Ubuntu 20.04 Clang 11 | [![Build Status](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_apis/build/status/Verona%20Nightly?branchName=master&stageName=Linux%20(Verona)&jobName=Linux&configuration=Linux%20Clang%20Release)](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_build/latest?definitionId=11&branchName=master) | [![Build Status](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_apis/build/status/Verona%20Nightly?branchName=master&stageName=Linux%20(Verona)&jobName=Linux&configuration=Linux%20Clang%20Debug)](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_build/latest?definitionId=11&branchName=master) | [![Build Status](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_apis/build/status/Verona%20Nightly?branchName=master&stageName=Linux%20(Verona)&jobName=Linux&configuration=Linux%20Clang%20Debug%20(SAN))](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_build/latest?definitionId=11&branchName=master)
Ubuntu 20.04 GCC 9 | [![Build Status](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_apis/build/status/Verona%20Nightly?branchName=master&stageName=Linux%20(Verona)&jobName=Linux&configuration=Linux%20GCC%20Release)](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_build/latest?definitionId=11&branchName=master) | [![Build Status](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_apis/build/status/Verona%20Nightly?branchName=master&stageName=Linux%20(Verona)&jobName=Linux&configuration=Linux%20GCC%20Debug)](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_build/latest?definitionId=11&branchName=master)
Windows 10 MSVC 2019 | [![Build Status](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_apis/build/status/Verona%20Nightly?branchName=master&stageName=Windows%20(Verona)&jobName=Windows&configuration=Windows%20Release)](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_build/latest?definitionId=11&branchName=master) | [![Build Status](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_apis/build/status/Verona%20Nightly?branchName=master&stageName=Windows%20(Verona)&jobName=Windows&configuration=Windows%20RelWithDebInfo)](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_build/latest?definitionId=11&branchName=master) |
MacOS 10.14 Clang 11 | [![Build Status](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_apis/build/status/Verona%20Nightly?branchName=master&stageName=macOS%20(Verona)&jobName=macOS&configuration=macOS%20Release)](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_build/latest?definitionId=11&branchName=master) | [![Build Status](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_apis/build/status/Verona%20Nightly?branchName=master&stageName=macOS%20(Verona)&jobName=macOS&configuration=macOS%20Debug)](https://dev.azure.com/ProjectVeronaCI/Project%20Verona/_build/latest?definitionId=11&branchName=master) |

# Syntax Highlight

Due to some amazing early contributors, we have syntax highlighting for the following editors:
* **VS Code:** Automatically suggests installing when opening Verona files. Thanks to @KodrAus (see #145).
* **Emacs:** [LINK](https://github.com/damon-kwok/verona-mode) Thanks to @damon-kwok (see #199).
* **Vim:** [LINK](https://github.com/timmyjose-projects/verona.vim) Thanks to @timmyjose (see #504).

## Development Documents

||||||
|--|--|--|--|--|
|[Exploring the language](docs/explore.md)|[FAQ](docs/faq.md)|[Building](docs/building.md)|[Contributing](CONTRIBUTING.md)|[Internal Design](docs/internal)|

**Warning:** These documents are work-in-progress and may change without notice.

<br>

<details><summary><h2> See Our Contributors 😄 </h2></summary>

<a href="https://github.com/microsoft/verona-rt/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=microsoft/verona-rt" />
</a>

</details>
