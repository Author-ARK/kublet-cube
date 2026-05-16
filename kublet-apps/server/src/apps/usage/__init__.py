from .claude import get_usage_data

ROUTES = {
    "/api/usage": get_usage_data,
}
