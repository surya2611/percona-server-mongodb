#!/usr/bin/env python3
"""Command line utility for generating suites for targeting antithesis."""

import os.path
import sys

import click
import yaml

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

HOOKS_BLACKLIST = [
    "CleanEveryN",
    "ContinuousStepdown",
    "CheckOrphansDeleted",
]

_SUITES_PATH = os.path.join("buildscripts", "resmokeconfig", "suites")


def delete_archival(suite):
    """Remove archival for Antithesis environment."""
    suite.pop("archive", None)
    suite.get("executor", {}).pop("archive", None)


def make_hooks_compatible(suite):
    """Make hooks compatible in Antithesis environment."""
    if suite.get("executor", {}).get("hooks", None):
        # it's either a list of strings, or a list of dicts, each with key 'class'
        if isinstance(suite["executor"]["hooks"][0], str):
            suite["executor"]["hooks"] = ["AntithesisLogging"] + [
                hook for hook in suite["executor"]["hooks"] if hook not in HOOKS_BLACKLIST
            ]
        elif isinstance(suite["executor"]["hooks"][0], dict):
            suite["executor"]["hooks"] = [{"class": "AntithesisLogging"}] + [
                hook for hook in suite["executor"]["hooks"] if hook["class"] not in HOOKS_BLACKLIST
            ]
        else:
            raise RuntimeError('Unknown structure in hook. File a TIG ticket.')


def use_external_fixture(suite):
    """Use external version of this fixture."""
    if suite.get("executor", {}).get("fixture", None):
        suite["executor"]["fixture"] = {
            "class": f"External{suite['executor']['fixture']['class']}",
            "shell_conn_string": "mongodb://mongos:27017"
        }


def update_test_data(suite):
    """Update TestData to be compatible with antithesis."""
    suite.setdefault("executor", {}).setdefault(
        "config", {}).setdefault("shell_options", {}).setdefault("global_vars", {}).setdefault(
            "TestData", {}).update({"useStepdownPermittedFile": False})


def update_shell(suite):
    """Update shell for when running in Antithesis."""
    suite.setdefault("executor", {}).setdefault("config", {}).setdefault("shell_options",
                                                                         {}).setdefault("eval", "")
    suite["executor"]["config"]["shell_options"]["eval"] += "jsTestLog = Function.prototype;"


def update_exclude_tags(suite):
    """Update the exclude tags to exclude antithesis incompatible tests."""
    suite.setdefault('selector', {}).setdefault('exclude_with_any_tags',
                                                []).append("antithesis_incompatible")


def make_suite_antithesis_compatible(suite):
    """Modify suite in-place to be antithesis compatible."""
    delete_archival(suite)
    make_hooks_compatible(suite)
    use_external_fixture(suite)
    update_test_data(suite)
    update_shell(suite)
    update_exclude_tags(suite)


@click.group()
def cli():
    """CLI Entry point."""
    pass


def _generate(suite_name: str) -> None:
    with open(os.path.join(_SUITES_PATH, f"{suite_name}.yml")) as fstream:
        suite = yaml.safe_load(fstream)

    make_suite_antithesis_compatible(suite)

    out = yaml.dump(suite)
    with open(os.path.join(_SUITES_PATH, f"antithesis_{suite_name}.yml"), "w") as fstream:
        fstream.write(
            "# this file was generated by buildscripts/antithesis_suite.py generate {}\n".format(
                suite_name))
        fstream.write("# Do not modify by hand\n")
        fstream.write(out)


@cli.command()
@click.argument('suite_name')
def generate(suite_name: str) -> None:
    """Generate a single suite."""
    _generate(suite_name)


@cli.command('generate-all')
def generate_all():
    """Generate all suites."""
    for path in os.listdir(_SUITES_PATH):
        if os.path.isfile(os.path.join(_SUITES_PATH, path)):
            suite = path.split(".")[0]
            _generate(suite)


if __name__ == "__main__":
    cli()
