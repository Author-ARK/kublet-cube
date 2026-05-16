from .hub import get_register, get_push, register

ROUTES = {
    "/api/notice/register": get_register,
    "/api/notice/push": get_push,
}
