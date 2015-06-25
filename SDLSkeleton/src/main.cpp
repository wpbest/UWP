#include <SDL.h>

SDL_Window * window = NULL;
SDL_Renderer * renderer = NULL;

void AppUpdate(void *userdata)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        /* Process SDL events here, as needed. */
    }

    /* Draw content: */
    SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
}

int main(int argc, char *argv[])
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL initialization failed: %s", SDL_GetError());
        return 1;
    }

    window = SDL_CreateWindow("SDL App", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!window) {
        SDL_Log("Unable to create SDL_Window: %s", SDL_GetError());
        return 1;
    }

    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        SDL_Log("Unable to create SDL_Renderer: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        window = NULL;
    }

    while (true) {
        AppUpdate(NULL);
    }

    return 0;
}
