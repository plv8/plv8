# PLV8 Versioning Policy

PLV8 generally follows [Semantic Versioning](https://semver.org) with some PLV8 specific changes.

Support for major and minor versions of PLV8 ends three (3) years after the release of the following version. On a case-by-case basis, support will be extended for back-ported regressions.

### Determining your PLV8 Version

You can use two ways to determine which version of PLV8 you are using:

```sql
$ SELECT plv8_version();
```

and

```sql
$ DO $$ plv8.elog(NOTICE, plv8.version); $$ LANGUAGE plv8;
```

## Major Version

Major versions increments with major changes in the codebase that could affect users and their code in a significant manner. These would include changes such as:

- Per user v8 isolates
- BigInt support
- JSONB conversion changes
- Major v8 changes

These types of changes can have a major impact on code execution, and thus part of a major version increase.

## Minor Version

Minor versions increments when there is a possibility of changes that can affect execution, but that where the amount of code changed is minimal and thus those changes have minimal impact on user code. Changes would include:

- Minor v8 changes (that require few modifications)
- Feature changes (behind feature compilation flags) and refinements

The goal is to avoid disruptive changes between minor releases.

## Patch Versions

Patch versions increment with a bug fix or minor improvement. Some minor version releases will include experimental code enabled at compile-time, disabled by default.

## PLV8 Version Support

PLV8 versions are generally supported at the minor release level for three (3) years after release.

| PLV8 Version | PLV8 Release Date | End of PLV8 Support Date |
| ------------ | ----------------- | ------------------------ |
| 3.2          | 2023-08-01        | 2026-08-01               |
| 3.1          | 2022-03-17        | 2025-03-17               |
| 3.0          | 2021-05-31        | 2024-05-31               |
| 2.3          | 2018-02-12        | 2021-02-12               |
| 2.1          | 2017-07-05        | 2020-07-05               |

## Bugs

In general, bugs are fixed with each minor release of PLV8.

### Back-porting of Patches

Back-porting of patches to a previous minor version is on a case-by-case basis. Generally if a version of PLV8 is beyond its support window, and not generally offered, then no back-porting of bug fixes will occur.

## Postgres Version Support

PLV8 maintains Postgres support for the current four (4) major releases of Postgres at a PLV8 minor version level.

This means that when PLV8 3.2.0 was released, just prior to Postgres 16 being released, PLV8 supported Postgres versions 12, 13, 14, 15, and 16 (which had not been yet released).
