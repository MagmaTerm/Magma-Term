#include <pty.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include <poll.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include <magma/logger/log.h>
#include <magma/backend/backend.h>

/* TODO: This entire file needs a 
 * redo and the structures need 
 * complete overall to be honest
 */

typedef struct pty {
	int master, slave;
} pty_t;

typedef struct magmatty {
	pty_t pty;

	unsigned char buf[10560];
	int use;

	magma_backend_t *backend;

	FcConfig *fcconfig;

	FT_Library library;
	FT_Face face;

	uint32_t maxascent, maxdescent;

	uint32_t width, height, xpos, ypos;

	bool is_running;
} magmatty_t;

int pty_get_master_slave(int *pmaster, int *pslave) {
	/*Should we use openpt?*/
	return openpty(pmaster, pslave, NULL, NULL, NULL);

};

pid_t fork_pty_pair(int master, int slave) {
	pid_t pid;

	const char *argv[] = { "/bin/sh", NULL };
	const char *envp[] = { "TERM=xterm", NULL };
	
	pid = fork();
	if(pid == 0) {
		close(master);


		setsid();
		if(ioctl(slave, TIOCSCTTY, NULL) == -1) {
			printf("IOCTL:(TIOCSCTTY) %m\n");
			return -1;
		}

		/*Dup slave as stdin, stdout, stderr*/
		for(int i = 0; i < 3; i++) {
			dup2(slave, i);
		}
		
		execve(argv[0], (char**)argv, (char**)envp);
		return -1;
	} else if(pid > 0) {
		close(slave);
		return pid;
	}

	printf("fork: %m\n");
	return -1;
}

int glyph_check_bit(const FT_GlyphSlot glyph, const int x, const int y)
{
    int pitch = glyph->bitmap.pitch;
    unsigned char *row = &glyph->bitmap.buffer[pitch * y];
    char cValue = row[x >> 3];

    return (cValue & (128 >> (x & 7))) != 0;
}

static int xpos = 0;
static int ypos = 0;
void echo_char(magmatty_t *ctx, int ch, uint32_t width, unsigned int maxAscent, uint32_t *data2) {
	FT_Bitmap *bitmap;
	if(ch == '\r') {
		return;
	}
	if(ch == '\n') {
		ypos += 24;
		xpos = 0;
		return;
	} 
	if(ch == 0x9) {
		FT_Load_Glyph(ctx->face, ' ', FT_LOAD_DEFAULT);
		uint32_t xposred = xpos / 14;
		
		xposred = ((xposred) | (8 - 1)) + 1; 
		
		xpos = xposred * 14;
		return;
	}
	FT_UInt glyphindex = FT_Get_Char_Index(ctx->face, ch);


	FT_Load_Glyph(ctx->face, glyphindex, FT_LOAD_DEFAULT);
	FT_Render_Glyph(ctx->face->glyph, FT_RENDER_MODE_MONO);
	
	bitmap = &ctx->face->glyph->bitmap;

	for(int y = 0; y < bitmap->rows; y++) {
		for(int x = 0; x < bitmap->width; x++) {
			if(glyph_check_bit(ctx->face->glyph, x, y)) {
				if(xpos + (ctx->face->glyph->advance.x >> 6) >= width) {
					ypos += 13;
					xpos = 0;
				}
				data2[(40 + y + ypos + (maxAscent - ctx->face->glyph->bitmap_top)) * width + 40 + x + xpos + ctx->face->glyph->bitmap_left] = 0xfff8f8f2;
			}
		}
	}
	xpos += ctx->face->glyph->advance.x >> 6;
}

uint16_t utf8_to_utf16(uint16_t b1, uint16_t b2, uint16_t b3) {
	if((b1 & 0x80) == 0x00) {
		return b1;
	} else if((b1 & 0xe0) == 0xc0) {
		return ((b1 & 0x1f) << 6) | (b2 & 0x3f);
	}
	return ((b1 & 0x0f) << 12) | ((b2 & 0x3f) << 6) | (b3 & 0x3f);
}

void draw_cb(magma_backend_t *backend, uint32_t height, uint32_t width, void *data) {
	if(width == 0) return;

	magmatty_t *ctx = data;
	uint32_t *data2;
	magma_buf_t buf = {
		.width = width,
		.height = height,
		.bpp = 32,
		.depth = 24,
		.buffer = NULL,
	};

	xpos = 0;
	ypos = 0;

	buf.buffer = calloc(4, width * height);
	data2 = buf.buffer;
	for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
			data2[y * width + x] = 0xa02a3628;
        }
    }

	for(int i = 0; i < ctx->use; i++) {
		if((ctx->buf[i] & 0xf0) == 0xe0) {
			uint16_t utf16 = utf8_to_utf16(ctx->buf[i], ctx->buf[i + 1], ctx->buf[i + 2]);
			echo_char(ctx, utf16, width, ctx->maxascent, data2);
			i++;
			i++;
		} else if((ctx->buf[i] & 0xe0) == 0xc0) {
			uint16_t utf16 = utf8_to_utf16(ctx->buf[i], ctx->buf[i + 1], 0);
			echo_char(ctx, utf16, width, ctx->maxascent, data2);
			i++;
		} else {
			echo_char(ctx, (ctx->buf[i]), width, ctx->maxascent, data2);
		}
	}
	magma_backend_put_buffer(backend, &buf);
}

void key_cb(magma_backend_t *backend, char *utf8, int length, void *data){
	magmatty_t *ctx = data;
	
	if(utf8[0] == 0x08) {
		write(ctx->pty.master, "\177", 1);
	} else {
		write(ctx->pty.master, utf8, length);
	}

	draw_cb(backend, ctx->height, ctx->height, data);
}

void fc_get_ascent_and_descent(magmatty_t *ctx) {
	FT_GlyphSlot glyph;
	for(int i = 32; i < UINT16_MAX; i++) {
		FT_UInt glyphindex = FT_Get_Char_Index(ctx->face, i);
		FT_Load_Glyph(ctx->face, glyphindex, FT_LOAD_DEFAULT);
		FT_Render_Glyph(ctx->face->glyph, FT_RENDER_MODE_NORMAL);
		glyph = ctx->face->glyph;

		if(ctx->maxascent < ctx->face->glyph->bitmap_top) {
			ctx->maxascent = ctx->face->glyph->bitmap_top;
		}
		if ((glyph->metrics.height >> 6) - glyph->bitmap_top > (int)ctx->maxdescent) {
    		ctx->maxdescent = (glyph->metrics.height >> 6) - glyph->bitmap_top;
		}
	}

}


void resize_cb(magma_backend_t *backend, uint32_t height, uint32_t width, void *data) {
	magmatty_t *ctx = data;
	struct winsize ws;
	ws.ws_xpixel = width;
	ws.ws_ypixel = height;
	ws.ws_col = width / 14;
	ws.ws_row = height / 14;
	
	ctx->width = width;
	ctx->height = height;

	printf("COL: %d ROW: %d\n", ws.ws_col, ws.ws_row);

	if(ioctl(ctx->pty.master, TIOCSWINSZ, &ws)) {
		printf("Failed to update term size\n");
	}
}

void font_init(magmatty_t *ctx, char *font) {
	char *font_file;
	FcPattern *pattern, *font_pattern;
	FcResult result;
	FcChar8 *fc_file;

	ctx->fcconfig = FcInitLoadConfigAndFonts();
	pattern = FcNameParse((const FcChar8*)font);
	
	FcConfigSubstitute(ctx->fcconfig, pattern, FcMatchPattern);
	FcDefaultSubstitute(pattern);
	
	font_pattern = FcFontMatch(ctx->fcconfig, pattern, &result);
	
	if(FcPatternGetString(font_pattern, FC_FILE, 0, &fc_file) == FcResultMatch) {
		
		font_file = (char *)fc_file;
	} else {
		return;
	}
	
	FT_Init_FreeType(&ctx->library);
	FT_New_Face(ctx->library, font_file, 0, &ctx->face);
	

	FcPatternDestroy(font_pattern);
	FcPatternDestroy(pattern);
}

void on_close(magma_backend_t *backend, void *data) {
	magmatty_t *ctx = data;


	ctx->is_running = 0;
}

int main(int argc, char **argv) {
	magmatty_t ctx = { 0 };
	struct pollfd pfd;

	if(pty_get_master_slave(&ctx.pty.master, &ctx.pty.slave) < 0) {
		return -1;
	}

	ctx.width = 0;
	ctx.height = 0;
	ctx.is_running = 1;	
	font_init(&ctx, "NotoMono Nerd Font-13px");

	fc_get_ascent_and_descent(&ctx);

	FT_Set_Pixel_Sizes(ctx.face, 24, 24);
	fork_pty_pair(ctx.pty.master, ctx.pty.slave);

	ctx.backend = magma_backend_init_auto();

	magma_backend_set_on_resize(ctx.backend, resize_cb, &ctx);
	magma_backend_set_on_draw(ctx.backend, draw_cb, &ctx);
	magma_backend_set_on_key(ctx.backend, key_cb, &ctx);
	magma_backend_set_on_close(ctx.backend, on_close, &ctx);

	magma_backend_start(ctx.backend);

	pfd.fd = ctx.pty.master;
	pfd.events = POLLIN;
	while(ctx.is_running) {
		magma_backend_dispatch_events(ctx.backend);
	
		if(poll(&pfd, 1, 10)) {
			if(pfd.revents & POLLERR || pfd.revents & POLLHUP) {
				printf("Child is process has closed\n");
				ctx.is_running = 0;
			}

			read(ctx.pty.master, &ctx.buf[ctx.use], 1);
			if(ctx.buf[ctx.use] == 0x1b) {
				char seq[2];
				read(ctx.pty.master, seq, 2);
				printf("Sequence 2: %d\n", seq[1]);
				if(seq[1] >= '0' && seq[1] <= '9') {
					char fin;
					read(ctx.pty.master, &fin, 1);
					if(fin == 'J') {
						ctx.use = 0;
					}
				}
				magma_log_debug("ESCAPE CODE: %d\n", 4);
				
			} else if (ctx.buf[ctx.use] != 0x8) {
				ctx.use++;
			} else {
				ctx.use--;
			}
		}
		draw_cb(ctx.backend, ctx.height, ctx.width, &ctx);
	}

	FILE *fp = fopen("tbuf.txt", "w");
	fwrite(ctx.buf, 1, ctx.use, fp);

	fclose(fp);

	magma_backend_dispatch_events(ctx.backend);
	magma_backend_deinit(ctx.backend);
	
	FT_Done_Face(ctx.face);
	FT_Done_FreeType(ctx.library);
	
	FcConfigDestroy(ctx.fcconfig);
	FcFini();
	return 0;
}
