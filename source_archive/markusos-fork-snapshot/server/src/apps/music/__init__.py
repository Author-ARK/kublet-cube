from .tracks import get_music_data, get_music_artwork

ROUTES = {
    "/api/music": get_music_data,
    "/api/music/artwork": get_music_artwork,
}
