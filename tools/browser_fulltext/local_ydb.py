"""Connect to the local_ydb deployment recorded in its recipe."""

from contextlib import contextmanager
import json
from pathlib import Path


RECIPE = Path.home() / "ydb-working-dir/ydb_recipe.json"


class LocalYdb:
    def __init__(self):
        node = next(iter(json.loads(RECIPE.read_text())["nodes"].values()))
        self.endpoint = f"grpc://{node['host']}:{node['grpc_port']}"
        self.database = RECIPE.with_name("ydb_database.txt").read_text().strip()

    @contextmanager
    def session_pool(self):
        from ydb import AnonymousCredentials, Driver, Error, QuerySessionPool

        try:
            with Driver(endpoint=self.endpoint, database=self.database, credentials=AnonymousCredentials()) as driver:
                driver.wait(timeout=10, fail_fast=True)
                with QuerySessionPool(driver) as pool:
                    yield pool
        except Error as exc:
            raise RuntimeError(str(exc)) from exc
