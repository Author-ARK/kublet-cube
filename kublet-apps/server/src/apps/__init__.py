"""App auto-discovery. Each sub-package exports ROUTES and optionally register()."""

import importlib
import pkgutil


def discover():
    """Return (routes_dict, register_fns_list) by scanning sub-packages."""
    routes = {}
    register_fns = []

    for _importer, modname, ispkg in pkgutil.iter_modules(__path__):
        if not ispkg:
            continue
        mod = importlib.import_module(f".{modname}", __package__)
        if hasattr(mod, "ROUTES"):
            routes.update(mod.ROUTES)
        if hasattr(mod, "register"):
            register_fns.append((modname, mod.register))

    return routes, register_fns
