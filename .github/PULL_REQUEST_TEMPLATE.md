## Summary

<!-- 1-3 sentences: what does this change and why. Link the issue if
there is one. -->

## Test plan

- [ ] `cmake --build build -j` builds clean
- [ ] `ctest --test-dir build --output-on-failure` passes
- [ ] Verified manually against a real PiKVM (describe how, including
      kvmd/ustreamer versions and which transport)
- [ ] CHANGELOG.md `[Unreleased]` entry added if user-visible
- [ ] DESIGN.md §10 updated if this commit reveals a new empirical
      contradiction with the design

## Related issues

<!-- e.g. Closes #12, refs #34 -->
