// zgloom.cpp : Defines the entry point for the console application.
//
#include <psp2/sysmodule.h>
#include <psp2/kernel/clib.h>
#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/dirent.h>

#include <stdio.h>
#include <xmp.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include "config.h"
#include "gloommap.h"
#include "script.h"
#include "crmfile.h"
#include "iffhandler.h"
#include "renderer.h"
#include "objectgraphics.h"
#include <iostream>
#include "gamelogic.h"
#include "soundhandler.h"
#include "font.h"
#include "titlescreen.h"
#include "menuscreen.h"
#include "hud.h"
#include <fstream>

Uint32 my_callbackfunc(Uint32 interval, void *param)
{
	SDL_Event event;
	SDL_UserEvent userevent;

	/* In this example, our callback pushes an SDL_USEREVENT event
	into the queue, and causes our callback to be called again at the
	same interval: */

	userevent.type = SDL_USEREVENT;
	userevent.code = 0;
	userevent.data1 = NULL;
	userevent.data2 = NULL;

	event.type = SDL_USEREVENT;
	event.user = userevent;

	SDL_PushEvent(&event);
	return (interval);
}

static void fill_audio(void *udata, Uint8 *stream, int len)
{
	auto res = xmp_play_buffer((xmp_context)udata, stream, len, 0);
}

void LoadPic(std::string name, SDL_Surface *render8)
{
	std::vector<uint8_t> pic;
	CrmFile picfile;
	CrmFile palfile;

	picfile.Load(name.c_str());
	palfile.Load((name + ".pal").c_str());

	SDL_FillRect(render8, nullptr, 0);

	// is this some sort of weird AGA/ECS backwards compatible palette encoding? 4 MSBs, then LSBs?
	// Update: Yes, yes it is.
	for (uint32_t c = 0; c < palfile.size / 4; c++)
	{
		SDL_Color col;
		col.a = 0xFF;
		col.r = palfile.data[c * 4 + 0] & 0xf;
		col.g = palfile.data[c * 4 + 1] >> 4;
		col.b = palfile.data[c * 4 + 1] & 0xF;

		col.r <<= 4;
		col.g <<= 4;
		col.b <<= 4;

		col.r |= palfile.data[c * 4 + 2] & 0xf;
		col.g |= palfile.data[c * 4 + 3] >> 4;
		col.b |= palfile.data[c * 4 + 3] & 0xF;

		SDL_SetPaletteColors(render8->format->palette, &col, c, 1);
	}

	uint32_t width = 0;

	IffHandler::DecodeIff(picfile.data, pic, width);

	if (width == render8->w)
	{
		if (pic.size() > (size_t)(render8->w * render8->h))
		{
			pic.resize(render8->w * render8->h);
		}
		std::copy(pic.begin(), pic.begin() + pic.size(), (uint8_t *)(render8->pixels));
	}
	else
	{
		// gloom 3 has some odd-sized intermission pictures. Do a line-by-line copy.

		uint32_t p = 0;
		uint32_t y = 0;

		if (pic.size() > (width * render8->h))
		{
			pic.resize(width * render8->h);
		}

		while (p < pic.size())
		{
			std::copy(pic.begin() + p, pic.begin() + p + render8->w, (uint8_t *)(render8->pixels) + y * render8->pitch);

			p += width;
			y++;
		}
	}
}

enum GameState
{
	STATE_PLAYING,
	STATE_PARSING,
	STATE_SPOOLING,
	STATE_WAITING,
	STATE_MENU,
	STATE_TITLE
};

void vgl_file_log(const char *format, ...)
{
	__gnuc_va_list arg;
	va_start(arg, format);
	char msg[512];
	vsnprintf(msg, sizeof(msg), format, arg);
	va_end(arg);
	FILE *log = fopen("ux0:/data/vitaGL.log", "a+");
	if (log != NULL)
	{
		fwrite(msg, 1, strlen(msg), log);
		fclose(log);
	}
}

int main(int argc, char *argv[])
{
	sceClibPrintf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
	// define vars for log and path variable 'selectedGame'
	char buffer[2048];
	memset(buffer, 0, 2048);

	// Check if any params are given
	sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
	SceAppUtilAppEventParam eventParam;
	sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
	sceAppUtilReceiveAppEvent(&eventParam);
	sceClibPrintf("\nEventType:%d\n",eventParam.type);
	if (eventParam.type == 0x05)
	{
		sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
		// set the appropriate game paths defined by param in Livearea - else launch Gloom Classic
		if (strstr(buffer, "deluxe"))
		{
			sceClibPrintf("\ndeluxe");
			Config::SetGame(Config::GameTitle::DELUXE);
		}
		else if (strstr(buffer, "gloom3"))
		{
			sceClibPrintf("\ngloom3");
			Config::SetGame(Config::GameTitle::GLOOM3);
		}
		else if (strstr(buffer, "massacre"))
		{
			sceClibPrintf("\nmassacre");
			Config::SetGame(Config::GameTitle::MASSACRE);
		}
	} else {
		sceClibPrintf("default\n");
		Config::SetGame(Config::GameTitle::GLOOM);
	}

	if (int dirID = sceIoDopen((Config::GetGamePath() ).c_str()) >= 0) // check if selected game dir exist
	{
		sceClibPrintf("\ngame dir exist:%s", Config::GetGamePath() .c_str());
		sceIoDclose(dirID);
	}
	else
	{
		sceClibPrintf("\ngame dir does not exist:%s , dirID:%d\n", Config::GetGamePath() .c_str(),dirID);
		sceIoDclose(dirID);
		return 0;
	}

	// Log file output to be sure params are being read
	//	vgl_file_log("\n---ZGLOOM");
	//	vgl_file_log("\nbuffer: %s", buffer);
	//	vgl_file_log("\nstring: %s", GetGamePath() .c_str());
	//	vgl_file_log("\nstring: %s", Config::isZM.c_str());

//	FILE *dbgFile = fopen("ux0:/data/ZGloom/debug.txt", "w");
	/* AUTODETECT ZM FIRST!*/
	if (strstr(buffer, "massacre")) {
		if (FILE *file = fopen("ux0:/data/ZGloom/massacre/stuf/stages", "r")) // check if ZM directory is existing
		{
//			fputs("detected ZM", dbgFile);
//			fclose(file);
			Config::SetZM(true);
		}
	}

	if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
	{
		std::cout << "SDL_Init Error: " << SDL_GetError() << std::endl;
		return 1;
	}

	// SDL needs to be inited before this to pick up gamepad
	Config::Init();

	GloomMap gmap;
	Script script;
	TitleScreen titlescreen;
	MenuScreen menuscreen;
	GameState state = STATE_TITLE;

	xmp_context ctx;

	ctx = xmp_create_context();
	Config::RegisterMusContext(ctx);

	int renderwidth, renderheight, windowwidth, windowheight;

	Config::GetRenderSizes(renderwidth, renderheight, windowwidth, windowheight);

	CrmFile titlemusic;
	CrmFile intermissionmusic;
	CrmFile ingamemusic;
	CrmFile titlepic;

	titlemusic.Load(Config::GetMusicFilename(0).c_str());
	intermissionmusic.Load(Config::GetMusicFilename(1).c_str());

	SoundHandler::Init();

	SDL_Window *win = SDL_CreateWindow("ZGloom", 100, 100, windowwidth, windowheight, SDL_WINDOW_SHOWN | (Config::GetFullscreen() ? SDL_WINDOW_FULLSCREEN : 0));
	if (win == nullptr)
	{
		std::cout << "SDL_CreateWindow Error: " << SDL_GetError() << std::endl;
		return 1;
	}

	Config::RegisterWin(win);

	SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | (Config::GetVSync() ? SDL_RENDERER_PRESENTVSYNC : 0));
	if (ren == nullptr)
	{
		std::cout << "SDL_CreateRenderer Error: " << SDL_GetError() << std::endl;
		return 1;
	}

	SDL_Texture *rendertex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, renderwidth, renderheight);
	if (rendertex == nullptr)
	{
		std::cout << "SDL_CreateTexture Error: " << SDL_GetError() << std::endl;
		return 1;
	}

	SDL_ShowCursor(SDL_DISABLE);

	SDL_Surface *render8 = SDL_CreateRGBSurface(0, 320, 256, 8, 0, 0, 0, 0);
	SDL_Surface *intermissionscreen = SDL_CreateRGBSurface(0, 320, 256, 8, 0, 0, 0, 0);
	SDL_Surface *titlebitmap = SDL_CreateRGBSurface(0, 320, 256, 8, 0, 0, 0, 0);
	SDL_Surface *render32 = SDL_CreateRGBSurface(0, renderwidth, renderheight, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
	SDL_Surface *screen32 = SDL_CreateRGBSurface(0, 320, 256, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);

	ObjectGraphics objgraphics;
	Renderer renderer;
	GameLogic logic;
	Camera cam;
	Hud hud;

	logic.Init(&objgraphics);
	SDL_AddTimer(1000 / 25, my_callbackfunc, NULL);

	SDL_Event sEvent;

	bool notdone = true;

#if 1
	Font smallfont, bigfont;
	CrmFile fontfile;
	fontfile.Load((Config::GetMiscDir() + "bigfont2.bin").c_str());
	if (fontfile.data)
	{
		bigfont.Load2(fontfile);
		smallfont.Load2(fontfile);
	}
	else
	{
		fontfile.Load((Config::GetMiscDir() + "smallfont.bin").c_str());
		if (fontfile.data)
			smallfont.Load(fontfile);
		fontfile.Load((Config::GetMiscDir() + "bigfont.bin").c_str());
		if (fontfile.data)
			bigfont.Load(fontfile);
	}
#endif

	std::string titlepicString = Config::GetPicsDir() + "title";
	bool success = titlepic.Load(titlepicString.c_str());
	sceClibPrintf("Success:%d, title pic dir:%s\n",success,Config::GetPicsDir().c_str());
	//return 0;

	if (titlepic.data)
	{
		LoadPic(Config::GetPicsDir() + "title", titlebitmap);

	}
	else
	{
		LoadPic(Config::GetPicsDir() + "spacehulk", titlebitmap);
		// LoadPic(Config::GetPicsDir() + "blackmagic", titlebitmap);
	}

	if (titlemusic.data)
	{
		if (xmp_load_module_from_memory(ctx, titlemusic.data, titlemusic.size))
		{
			std::cout << "music error";
		}

		if (xmp_start_player(ctx, 22050, 0))
		{
			std::cout << "music error";
		}
		Mix_HookMusic(fill_audio, ctx);
		Config::SetMusicVol(Config::GetMusicVol());
	}

	std::string intermissiontext;

	bool intermissionmusplaying = false;
	bool haveingamemusic = false;
	bool printscreen = false;
	int screennum = 0;
	uint32_t fps = 0;
	uint32_t fpscounter = 0;

	Mix_Volume(-1, Config::GetSFXVol() * 12);
	Mix_VolumeMusic(Config::GetMusicVol() * 12);

	//try and blit title etc into the middle of the screen
	SDL_Rect blitrect;

	int screenscale = renderheight / 256;
	blitrect.w = 320 * screenscale;
	blitrect.h = 256 * screenscale;
	blitrect.x = (renderwidth - 320 * screenscale) / 2;
	blitrect.y = (renderheight - 256 * screenscale) / 2;

	SDL_SetRelativeMouseMode(SDL_TRUE);

	//set up the level select

	std::vector<std::string> levelnames;
	script.GetLevelNames(levelnames);
	titlescreen.SetLevels(levelnames);
	int levelselect = 0;

	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	SceCtrlData inputData;

	Input::Init();
	while (notdone)
	{
		sceCtrlPeekBufferPositive(0, &inputData, 1);

		if ((state == STATE_PARSING) || (state == STATE_SPOOLING))
		{
			std::string scriptstring;
			Script::ScriptOp sop;

			sop = script.NextLine(scriptstring);

			switch (sop)
			{
			case Script::SOP_SETPICT:
			{
				scriptstring.insert(0, Config::GetPicsDir());
				LoadPic(scriptstring, intermissionscreen);
				SDL_SetPaletteColors(render8->format->palette, intermissionscreen->format->palette->colors, 0, 256);
				break;
			}
			case Script::SOP_SONG:
			{
				scriptstring.insert(0, Config::GetMusicDir());
				ingamemusic.Load(scriptstring.c_str());
				haveingamemusic = (ingamemusic.data != nullptr);
				break;
			}
			case Script::SOP_LOADFLAT:
			{
				//improve this, only supports 9 flats
				gmap.SetFlat(scriptstring[0] - '0');
				break;
			}
			case Script::SOP_TEXT:
			{
				intermissiontext = scriptstring;

				if (state == STATE_SPOOLING)
				{
					if (intermissiontext == levelnames[levelselect])
					{
						// level selector
						if (intermissionmusic.data)
						{
							if (xmp_load_module_from_memory(ctx, intermissionmusic.data, intermissionmusic.size))
							{
								std::cout << "music error";
							}

							if (xmp_start_player(ctx, 22050, 0))
							{
								std::cout << "music error";
							}
							Mix_HookMusic(fill_audio, ctx);
							Config::SetMusicVol(Config::GetMusicVol());
							intermissionmusplaying = true;
						}

						state = STATE_PARSING;
					}
				}
				break;
			}
			case Script::SOP_DRAW:
			{
				if (state == STATE_PARSING)
				{
					if (intermissionmusic.data)
					{
						if (xmp_load_module_from_memory(ctx, intermissionmusic.data, intermissionmusic.size))
						{
							std::cout << "music error";
						}

						if (xmp_start_player(ctx, 22050, 0))
						{
							std::cout << "music error";
						}
						Mix_HookMusic(fill_audio, ctx);
						Config::SetMusicVol(Config::GetMusicVol());
						intermissionmusplaying = true;
					}
				}
				break;
			}
			case Script::SOP_WAIT:
			{
				if (state == STATE_PARSING)
				{
					state = STATE_WAITING;

					SDL_SetPaletteColors(render8->format->palette, smallfont.GetPalette()->colors, 0, 16);
					SDL_BlitSurface(intermissionscreen, NULL, render8, NULL);
					smallfont.PrintMultiLineMessage(intermissiontext, 245, render8); // moved level text to black bar from 220 to 245, so whole height is used
				}
				break;
			}
			case Script::SOP_PLAY:
			{
				if (state == STATE_PARSING)
				{

					cam.x.SetInt(0);
					cam.y = 120;
					cam.z.SetInt(0);
					cam.rotquick.SetInt(0);
					scriptstring.insert(0, Config::GetLevelDir());
					gmap.Load(scriptstring.c_str(), &objgraphics);
					//gmap.Load("maps/map1_4", &objgraphics);
					renderer.Init(render32, &gmap, &objgraphics);
					logic.InitLevel(&gmap, &cam, &objgraphics);
					state = STATE_PLAYING;

					if (haveingamemusic)
					{
						if (xmp_load_module_from_memory(ctx, ingamemusic.data, ingamemusic.size))
						{
							std::cout << "music error";
						}

						if (xmp_start_player(ctx, 22050, 0))
						{
							std::cout << "music error";
						}
						Mix_HookMusic(fill_audio, ctx);
						Config::SetMusicVol(Config::GetMusicVol());
					}
				}
				break;
			}
			case Script::SOP_END:
			{
				state = STATE_TITLE;
				if (intermissionmusic.data && intermissionmusplaying)
				{
					Mix_HookMusic(nullptr, nullptr);
					xmp_end_player(ctx);
					xmp_release_module(ctx);
					intermissionmusplaying = false;
				}
				if (titlemusic.data)
				{
					if (xmp_load_module_from_memory(ctx, titlemusic.data, titlemusic.size))
					{
						std::cout << "music error";
					}

					if (xmp_start_player(ctx, 22050, 0))
					{
						std::cout << "music error";
					}
					Mix_HookMusic(fill_audio, ctx);
					Config::SetMusicVol(Config::GetMusicVol());
				}
				break;
			}
			}
		}

		if (state == STATE_TITLE)
		{
			SDL_SetPaletteColors(render8->format->palette, smallfont.GetPalette()->colors, 0, 16);		   // added for correct palette on titlescreen
			SDL_SetPaletteColors(render8->format->palette, titlebitmap->format->palette->colors, 17, 256); // 256-16 colors should be enough
			titlescreen.Render(titlebitmap, render8, smallfont);
		}

		while ((state != STATE_SPOOLING) && SDL_PollEvent(&sEvent))
		{
			Input::Update();

			if (sEvent.type == SDL_WINDOWEVENT)
			{
				if (sEvent.window.event == SDL_WINDOWEVENT_CLOSE)
				{
					notdone = false;
				}
			}

			if (Input::GetButtonDown(SCE_CTRL_CROSS))
			{
				if (state == STATE_WAITING)
				{
					state = STATE_PARSING;
					if (intermissionmusic.data)
					{
						Mix_HookMusic(nullptr, nullptr);
						xmp_end_player(ctx);
						xmp_release_module(ctx);
						intermissionmusplaying = false;
					}
				}
			}

			if (state == STATE_TITLE)
			{
				switch (titlescreen.Update(levelselect))
				{
				case TitleScreen::TITLERET_PLAY:
					state = STATE_PARSING;
					logic.Init(&objgraphics);
					if (titlemusic.data)
					{
						Mix_HookMusic(nullptr, nullptr);
						xmp_end_player(ctx);
						xmp_release_module(ctx);
					}
					break;
				case TitleScreen::TITLERET_SELECT:
					state = STATE_SPOOLING;
					logic.Init(&objgraphics);
					if (titlemusic.data)
					{
						Mix_HookMusic(nullptr, nullptr);
						xmp_end_player(ctx);
						xmp_release_module(ctx);
					}
					break;
				case TitleScreen::TITLERET_QUIT:
					notdone = false;
					break;
				default:
					break;
				}
			}

			if (state == STATE_MENU)
			{
				switch (menuscreen.Update())
				{
				case MenuScreen::MENURET_PLAY:
					state = STATE_PLAYING;
					break;
				case MenuScreen::MENURET_QUIT:
					script.Reset();
					state = STATE_TITLE;
					if (titlemusic.data)
					{
						if (xmp_load_module_from_memory(ctx, titlemusic.data, titlemusic.size))
						{
							std::cout << "music error";
						}

						if (xmp_start_player(ctx, 22050, 0))
						{
							std::cout << "music error";
						}
						Mix_HookMusic(fill_audio, ctx);
						Config::SetMusicVol(Config::GetMusicVol());
					}
					break;
				default:
					break;
				}
			}
			if ((state == STATE_PLAYING) && Input::GetButtonDown(SCE_CTRL_START))
			{
				state = STATE_MENU;
			}

			if ((state == STATE_PLAYING) && Input::GetButtonDown(SCE_CTRL_SELECT))
			{
				Config::SetDebug(!Config::GetDebug());
			}

			// if ((sEvent.type == SDL_KEYDOWN) && sEvent.key.keysym.sym == SDLK_PRINTSCREEN)
			// {
			// 	printscreen = true;
			// }

			if (sEvent.type == SDL_USEREVENT)
			{
				if (state == STATE_PLAYING)
				{
					if (logic.Update(&cam))
					{
						if (haveingamemusic)
						{
							Mix_HookMusic(nullptr, nullptr);
							xmp_end_player(ctx);
							xmp_release_module(ctx);
							intermissionmusplaying = false;
						}
						state = STATE_PARSING;
					}
				}
				if (state == STATE_TITLE)
				{
					titlescreen.Clock();
				}
				if (state == STATE_MENU)
				{
					menuscreen.Clock();
				}

				fpscounter++;

				if (fpscounter >= 25)
				{
					Config::SetFPS(fps);
					fpscounter = 0;
					fps = 0;
				}
			}
		}

		SDL_FillRect(render32, NULL, 0);

		if (state == STATE_PLAYING)
		{
			renderer.SetTeleEffect(logic.GetTeleEffect());
			renderer.SetPlayerHit(logic.GetPlayerHit());
			renderer.SetThermo(logic.GetThermo());

			//cam.x.SetInt(3969);
			//cam.z.SetInt(5359);
			//cam.rotquick.SetInt(254);
			renderer.Render(&cam);
			MapObject pobj = logic.GetPlayerObj();
			hud.Render(render32, pobj, smallfont);
			fps++;
		}
		if (state == STATE_MENU)
		{
			renderer.Render(&cam);
			menuscreen.Render(render32, render32, smallfont);
		}

		if ((state == STATE_WAITING) || (state == STATE_TITLE))
		{
			// SDL does not seem to like scaled 8->32 copy?
			SDL_BlitSurface(render8, NULL, screen32, NULL);
			SDL_BlitScaled(screen32, NULL, render32, &blitrect);
		}

		if (printscreen)
		{
			std::string filename("img");

			filename += std::to_string(screennum);
			filename += ".bmp";
			screennum++;

			SDL_SaveBMP(render32, filename.c_str());
			printscreen = false;
		}

		if (state != STATE_SPOOLING)
		{
			SDL_UpdateTexture(rendertex, NULL, render32->pixels, render32->pitch);
			SDL_RenderClear(ren);
			SDL_RenderCopy(ren, rendertex, NULL, NULL);
			SDL_RenderPresent(ren);
		}
	}

	xmp_free_context(ctx);

	Config::Save();

	SoundHandler::Quit();

	SDL_FreeSurface(render8);
	SDL_FreeSurface(render32);
	SDL_FreeSurface(screen32);
	SDL_FreeSurface(intermissionscreen);
	SDL_FreeSurface(titlebitmap);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();

	return 0;
}
