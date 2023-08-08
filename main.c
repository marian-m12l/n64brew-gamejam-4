#include "libdragon.h"
#include <malloc.h>
#include <math.h>

static sprite_t *brew_sprite;
static sprite_t *ball_sprite;
static sprite_t *net_sprite;
static sprite_t *tiles_sprite;

static rspq_block_t *tiles_block;

typedef struct {
    float x;
    float y;
} vector2d_t;

typedef struct {
    vector2d_t pos;
    vector2d_t dir;
    vector2d_t normalized;
    float length;
} collision_t;

typedef struct {
    float x;
    float y;
    float dx;
    float dy;
    float scale_factor; // TODO support separate x/y scale factors? support rotation?
} object_t;

#define NUM_BLOBS 2

static object_t blobs[NUM_BLOBS];
static object_t ball;
static object_t net;

static int scorePlayer1 = 0;
static int scorePlayer2 = 0;
static int lastPlayer = -1;
static int hitCount = 0;

static int mode = 1;

// Fair and fast random generation (using xorshift32, with explicit seed)
static uint32_t rand_state = 1;
static uint32_t rand(void) {
	uint32_t x = rand_state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 5;
	return rand_state = x;
}

// RANDN(n): generate a random number from 0 to n-1
#define RANDN(n) ({ \
	__builtin_constant_p((n)) ? \
		(rand()%(n)) : \
		(uint32_t)(((uint64_t)rand() * (n)) >> 32); \
})

bool rectRect(float r1x, float r1y, float r1w, float r1h, float r2x, float r2y, float r2w, float r2h) {
  return (r1x + r1w >= r2x &&    // r1 right edge past r2 left
          r1x <= r2x + r2w &&    // r1 left edge past r2 right
          r1y + r1h >= r2y &&    // r1 top edge past r2 bottom
          r1y <= r2y + r2h);     // r1 bottom edge past r2 top
}

collision_t circleRect(float cx, float cy, float radius, float rx, float ry, float rw, float rh) {
  float nearestX = cx;
  float nearestY = cy;

  // which edge is closest?
  if (cx < rx)         nearestX = rx;      // test left edge
  else if (cx > rx+rw) nearestX = rx+rw;   // right edge
  if (cy < ry)         nearestY = ry;      // top edge
  else if (cy > ry+rh) nearestY = ry+rh;   // bottom edge

  // get distance from closest edges
  float distX = cx - nearestX;
  float distY = cy - nearestY;
  float distance = sqrt( (distX*distX) + (distY*distY) );

  // if the distance is less than the radius, collision!
  vector2d_t pos = {nearestX, nearestY};
  vector2d_t dir = {distX, distY};
  vector2d_t normal = {0, 0};
  if (distance > 0 && distance <= radius) {
    normal.x = distX/distance;
    normal.y = distY/distance;
  }
  collision_t retval = {pos, dir, normal, distance};
  return retval;
}

static int32_t obj_min_x;
static int32_t obj_max_x;
static int32_t obj_min_y;
static int32_t obj_max_y;
static int32_t cur_tick = 0;

// DEBUG collisions
static collision_t collisions[NUM_BLOBS];

#define FRAMERATE 60
#define AIR_FRICTION_FACTOR 0.99f
#define GROUND_FRICTION_FACTOR 0.9f
#define GRAVITY_FACTOR 9.81f
#define SPEED_EPSILON 1e-1
#define POSITION_EPSILON 10

void applyScreenLimits(float x, float y, float w, float h, float dx, float dy, object_t* obj) {
    float next_x = x + dx;
    float next_y = y + dy;

    if (next_x + w >= obj_max_x) {
        next_x = obj_max_x - (next_x + w - obj_max_x) - w;
        obj->dx = -1.0 * dx;
        //fprintf(stderr, "X position %f >= %ld --> BOUNCE TO %f, dx=%f\n", next_x + w, obj_max_x, next_x, obj->dx);
    }
    if (next_x < obj_min_x) {
        next_x = obj_min_x + (obj_min_x - next_x);
        obj->dx = -1.0 * dx;
        //fprintf(stderr, "X position %f < %ld --> BOUNCE TO %f, dx=%f\n", next_x, obj_min_x, next_x, obj->dx);
    }
    if (next_y + h >= obj_max_y) {
        next_y = obj_max_y - (next_y + h - obj_max_y) - h;
        obj->dy = -1.0 * dy / 2;
        //fprintf(stderr, "Y position %f >= %ld --> BOUNCE TO %f, dy=%f\n", next_y + h, obj_max_y, next_y, obj->dy);
    }
    if (next_y < obj_min_y) {
        next_y = obj_min_y + (obj_min_x - next_y);
        obj->dy = -1.0 * dy;
        //fprintf(stderr, "Y position %f < %ld --> BOUNCE TO %f, dy=%f\n", next_y, obj_min_y, next_y, obj->dy);
    }
    
    obj->x = next_x;
    obj->y = next_y;
}

void applyScreenLimitsRect(object_t* obj, sprite_t* sprite) {
    applyScreenLimits(obj->x, obj->y, sprite->width, sprite->height, obj->dx, obj->dy, obj);
}

void applyScreenLimitsCircle(object_t* obj, sprite_t* sprite) {
    applyScreenLimits(obj->x - sprite->width/2, obj->y - sprite->height/2, sprite->width, sprite->height, obj->dx, obj->dy, obj);
    obj->x += sprite->width/2;
    obj->y += sprite->height/2;
}

void applyFriction(object_t* obj) {
    if (obj->dx != 0) {
        if (fabs(obj->dx) < SPEED_EPSILON) {
            fprintf(stderr, "dx < %f --> 0\n", SPEED_EPSILON);
            obj->dx = 0;
        } else {
            float factor = (obj->y < obj_max_y) ? AIR_FRICTION_FACTOR : GROUND_FRICTION_FACTOR;
            //fprintf(stderr, "applying friction...\n");
            float next_dx = fabs(obj->dx) * factor;
            //fprintf(stderr, "blob[%ld]: next_dx=%f obj->dx=%f/%f\n", i, next_dx, -1.0f * next_dx, next_dx);
            //fprintf(stderr, "blob[%ld]: x=%f dx=%f fabs(dx)=%f next_dx=%f\n", i, obj->x, obj->dx, fabs(obj->dx), (obj->dx < 0) ? (-1.0f * next_dx) : next_dx);
            if (obj->dx < 0) {
                obj->dx = -1.0f * next_dx;
            } else {
                obj->dx = next_dx;
            }
        }
    }
}

void applyGravity(object_t* obj) {
    if (obj->dy > 0 && obj->dy < SPEED_EPSILON && (obj_max_y - fabs(obj->y)) < POSITION_EPSILON) {
        fprintf(stderr, "dy < %f --> 0\n", SPEED_EPSILON);
        obj->dy = 0;
        obj->y = obj_max_y;
    } else if (obj->y != obj_max_y) {
        float next_dy = obj->dy + (GRAVITY_FACTOR / FRAMERATE);
        //fprintf(stderr, "blob[%ld]: y=%f dy=%f fabs(dy)=%f next_dy=%f\n", i, obj->y, obj->dy, fabs(obj->dy), next_dy);
        obj->dy = next_dy;
    }
}

void update(int ovfl)
{
    if (mode == 2) {
        return;
    }

    // Ball
    //fprintf(stderr, "Applying screen limits BALL\n");
    applyScreenLimitsCircle(&ball, ball_sprite);
    // TODO also air friction? magnus effect?
    applyFriction(&ball);
    applyGravity(&ball);

    // TODO Handle collision with net
    collision_t netCollision = circleRect(ball.x, ball.y, ball_sprite->width/2, net.x, net.y, net_sprite->width, net_sprite->height);
    vector2d_t netCollisionNormal = netCollision.normalized;
    if (netCollisionNormal.x != 0 || netCollisionNormal.y != 0) {
        // TODO Stop / bounce
        fprintf(stderr, "Ball/Net collision\n");

            // TODO Use (normalized) vector from player center to ball center instead of nearest collision poiint ???
            float distX = ball.x - (net.x + net_sprite->width/2);
            float distY = ball.y - (net.y + net_sprite->height/2);
            float distance = sqrt( (distX*distX) + (distY*distY) );
            //vector2d_t netBall = { distX, distY };
            vector2d_t netBallNormal = {
                distX/distance,
                distY/distance
            };
            netCollisionNormal = netBallNormal;

            fprintf(stderr, "NET/BALL COLLISION: normal=(%f, %f) ball=(%f,%f)(%f,%f) net=(%f,%f)(%f,%f)\n",
                netCollisionNormal.x, netCollisionNormal.y,
                ball.x, ball.y, ball.dx, ball.dy,
                net.x, net.y, net.dx, net.dy);
            // TODO if hitting on the side, reverse ball dx
            // TODO If hitting on top, reverse ball dy
            float next_ball_dx = (netCollision.pos.x == net.x || netCollision.pos.x == (net.x + net_sprite->width)) ? -1.0f * ball.dx : ball.dx;
            float next_ball_dy = (netCollision.pos.y == net.y) ? -1.0f *ball.dy : ball.dy;
            fprintf(stderr, "\tball.dx: %f --> %f\n", ball.dx, next_ball_dx);
            fprintf(stderr, "\tball.dy: %f --> %f\n", ball.dy, next_ball_dy);
            // TODO ball effects? lift/slice? + magnus effect when in flight???

            ball.dx = next_ball_dx;
            ball.dy = next_ball_dy;

            // TODO Resolve collisions --> move ball
            float next_ball_x = ball.x;
            float next_ball_y = ball.y;
            // TODO dependns on nearest X/Y ? if to the right, add x, if to the left, sub x, if to the top, add y if to the bottom, sub y
            if (netCollision.pos.x == net.x) {
                // Ball is on the left
                fprintf(stderr, "\tResolving collision by moving ball to the LEFT by: %f\n", ball_sprite->width/2 - fabs(netCollision.dir.x));
                next_ball_x -= ball_sprite->width/2 - fabs(netCollision.dir.x);
            } else if (netCollision.pos.x == (net.x + net_sprite->width)) {
                // Ball is on the right
                fprintf(stderr, "\tResolving collision by moving ball to the RIGHT by: %f\n", ball_sprite->width/2 - fabs(netCollision.dir.x));
                next_ball_x += ball_sprite->width/2 - fabs(netCollision.dir.x);
            } else if (netCollision.pos.y == net.y) {
                // Ball is on the top
                fprintf(stderr, "\tResolving collision by moving ball to the TOP by: %f\n", ball_sprite->height/2 - fabs(netCollision.dir.y));
                next_ball_y -= ball_sprite->height/2 - fabs(netCollision.dir.y);
            } else if (netCollision.pos.y == (net.y + net_sprite->height)) {
                // Ball is on the bottom
                fprintf(stderr, "\tResolving collision by moving ball to the BOTTOM by: %f\n", ball_sprite->height/2 - fabs(netCollision.dir.y));
                next_ball_y += ball_sprite->height/2 - fabs(netCollision.dir.y);
            }

            fprintf(stderr, "\tball.x: %f --> %f\n", ball.x, next_ball_x);
            fprintf(stderr, "\tball.y: %f --> %f\n", ball.y, next_ball_y);
            ball.x = next_ball_x;
            ball.y = next_ball_y;
    }

    // TODO When colliding with floor, stop point and increase score

    //fprintf(stderr, "update\n");
    for (uint32_t i = 0; i < NUM_BLOBS; i++)
    {
        object_t *obj = &blobs[i];
        //fprintf(stderr, "blob[%ld]: x=%ld y=%ld dx=%f dy=%f\n", i, obj->x, obj->y, obj->dx, obj->dy);

        //fprintf(stderr, "Applying screen limits PLAYER %ld\n", i);
        applyScreenLimitsRect(obj, brew_sprite); // FIXME Handle with collisions to be resolved all at once ?

        //fprintf(stderr, "blob[%ld]: x=%ld y=%ld dx=%f dy=%f\n", i, obj->x, obj->y, obj->dx, obj->dy);
        //fprintf(stderr, "blob[%ld]: fabs(dx)=%f\n", i, fabs(obj->dx));

        // Apply gravity / friction
        applyFriction(obj);
        applyGravity(obj);

        // TODO Handle collisions
            // Player / Net (bounce / block)
            // Player / Ball (up to 3 hits per turn)
            // Screen borders / Ball (bounce, loose some momentum)
            // Ground / Ball (end point)
            // Player / Bonus ??? (higher bounce, faster speed, move net down/up, ...)
            
        // FIXME DEBUG player/player collision NOT NEEDED
        /*if (rectRect(blobs[0].x, blobs[0].y, brew_sprite->width, brew_sprite->height, blobs[1].x, blobs[1].y, brew_sprite->width, brew_sprite->height)) {
            fprintf(stderr, "PLAYERS COLLISION\n");
        }*/

        // FIXME Player/net collision
        if (rectRect(obj->x, obj->y, brew_sprite->width, brew_sprite->height, net.x, net.y, net_sprite->width, net_sprite->height)) {
            fprintf(stderr, "Player / Net collision\n");
            // TODO Reposition player to the left/right of net
            if (obj->x < net.x) {
                obj->x = net.x - brew_sprite->width;
            } else {
                obj->x = net.x + net_sprite->width;
            }
        }
        
        // FIXME Ball collision
        collision_t collision = circleRect(ball.x, ball.y, ball_sprite->width/2, obj->x, obj->y, brew_sprite->width, brew_sprite->height);
        vector2d_t collisionNormal = collision.normalized;
        if ((collisionNormal.x != 0 || collisionNormal.y != 0) && !(lastPlayer == i && hitCount > 2)) {
            // TODO Use (normalized) vector from player center to ball center instead of nearest collision poiint ???
            float distX = ball.x - (obj->x + brew_sprite->width/2);
            float distY = ball.y - (obj->y + brew_sprite->height/2);
            float distance = sqrt( (distX*distX) + (distY*distY) );
            //vector2d_t playerBall = { distX, distY };
            vector2d_t playerBallNormal = {
                distX/distance,
                distY/distance
            };
            collisionNormal = playerBallNormal;

            fprintf(stderr, "PLAYER/BALL COLLISION: normal=(%f, %f) ball=(%f,%f)(%f,%f) obj=(%f,%f)(%f,%f)\n",
                collisionNormal.x, collisionNormal.y,
                ball.x, ball.y, ball.dx, ball.dy,
                obj->x, obj->y, obj->dx, obj->dy);
            // FIXME if player and ball velocity have opposite signs, ball velocity is inversed (rebound)
            // TODO should bounce even if obj is not moving !!!
            // TODO should depend on the ball position relative to the player ??
            float ball_dx_fixed = (ball.dx * obj->dx) >= 0 ? ball.dx : -ball.dx;
            float ball_dy_fixed = (ball.dy * obj->dy) >= 0 ? ball.dy : -ball.dy;
            float next_ball_dx = /*fabs(collisionNormal.x) * */(ball_dx_fixed + obj->dx);
            float next_ball_dy = /*fabs(collisionNormal.y) * */(ball_dy_fixed + obj->dy);
            fprintf(stderr, "\tball.dx: %f --> %f\n", ball.dx, next_ball_dx);
            fprintf(stderr, "\tball.dy: %f --> %f\n", ball.dy, next_ball_dy);
            // TODO Compute bounce vector from ball/player centers ???
            // TODO player's momentum should transfer to ball !!!
            // TODO ball effects? lift/slice? + magnus effect when in flight???

            // FIXME also bounce with ball velocity ???
            ball.dx = next_ball_dx;
            ball.dy = next_ball_dy;

            // TODO player's momentum also reduce by ball momentum (before hit)? --> add a weight factor ??
            //obj.dx *= .8;
            //obj.dy *= .8;

            // TODO: new ball dx = old ball dx **reverted if hitting from the side** --> multiplied by normal vector ??

            // TODO Resolve collisions --> move ball
            float next_ball_x = ball.x;
            float next_ball_y = ball.y;
            // TODO dependns on nearest X/Y ? if to the right, add x, if to the left, sub x, if to the top, add y if to the bottom, sub y
            if (collision.pos.x == obj->x) {
                // Ball is on the left
                fprintf(stderr, "\tResolving collision by moving ball to the LEFT by: %f\n", ball_sprite->width/2 - fabs(collision.dir.x));
                next_ball_x -= ball_sprite->width/2 - fabs(collision.dir.x);
            } else if (collision.pos.x == (obj->x + brew_sprite->width)) {
                // Ball is on the right
                fprintf(stderr, "\tResolving collision by moving ball to the RIGHT by: %f\n", ball_sprite->width/2 - fabs(collision.dir.x));
                next_ball_x += ball_sprite->width/2 - fabs(collision.dir.x);
            } else if (collision.pos.y == obj->y) {
                // Ball is on the top
                fprintf(stderr, "\tResolving collision by moving ball to the TOP by: %f\n", ball_sprite->height/2 - fabs(collision.dir.y));
                next_ball_y -= ball_sprite->height/2 - fabs(collision.dir.y);
            } else if (collision.pos.y == (obj->y + brew_sprite->height)) {
                // Ball is on the bottom
                fprintf(stderr, "\tResolving collision by moving ball to the BOTTOM by: %f\n", ball_sprite->height/2 - fabs(collision.dir.y));
                next_ball_y += ball_sprite->height/2 - fabs(collision.dir.y);
            }

            //float next_ball_x = ball.x + playerBall.x - ball_sprite->width/2 - brew_sprite->width/2;//collision.dir.x;//collisionNormal.x;
            //float next_ball_y = ball.y + playerBall.y;//collision.dir.y;//collisionNormal.y;
            fprintf(stderr, "\tball.x: %f --> %f\n", ball.x, next_ball_x);
            fprintf(stderr, "\tball.y: %f --> %f\n", ball.y, next_ball_y);
            ball.x = next_ball_x;
            ball.y = next_ball_y;

            // TODO draw normal vector?? bounding boxes???

            // TODO Max 3 hits per player
            if (lastPlayer != i) {
                lastPlayer = i;
                hitCount = 0;
            }
            hitCount++;
        }
        collisions[i] = collision;
        // TODO Resolve collisions
    }

    cur_tick++;
}

void render(int cur_frame, int mode)
{
    surface_t *disp = display_get();

    /*Fill the screen */
    graphics_fill_screen(disp, 0xFFFFFFFF);

    /* Set the text output color */
    graphics_set_color(0x0, 0xFFFFFFFF);
    
    if (mode == 0) {    // RDPQ
        // Attach and clear the screen
        graphics_draw_text( disp, 20, 20, "Mode 0: RDPQ" );

        rdpq_attach/*_clear*/(disp, NULL);

        // Draw the tile background, by playing back the compiled block.
        // This is using copy mode by default, but notice how it can switch
        // to standard mode (aka "1 cycle" in RDP terminology) in a completely
        // transparent way. Even if the block is compiled, the RSP commands within it
        // will adapt its commands to the current render mode, Try uncommenting
        // the line below to see.
        rdpq_debug_log_msg("tiles");
        rdpq_set_mode_copy(false);
        // rdpq_set_mode_standard();
        rspq_block_run(tiles_block);
        
        // Draw the brew sprites. Use standard mode because copy mode cannot handle
        // scaled sprites.
        rdpq_debug_log_msg("sprites");
        rdpq_set_mode_standard();
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_mode_alphacompare(1);                // colorkey (draw pixel with alpha >= 1)

        for (uint32_t i = 0; i < NUM_BLOBS; i++)
        {
            rdpq_sprite_blit(brew_sprite, (int32_t) blobs[i].x, (int32_t) blobs[i].y, &(rdpq_blitparms_t){
                .scale_x = blobs[i].scale_factor, .scale_y = blobs[i].scale_factor,
            });
        }

        // Ball
        rdpq_sprite_blit(ball_sprite, (int32_t) (ball.x - ball_sprite->width/2), (int32_t) (ball.y - ball_sprite->height/2), &(rdpq_blitparms_t){
            .scale_x = ball.scale_factor, .scale_y = ball.scale_factor,
        });

        rdpq_detach();//_show();
        
        graphics_draw_text( disp, 20, 20, "Mode 0: RDPQ" );
    } else if (mode == 1) {    // Sprites
        graphics_draw_text( disp, 20, 20, "Mode 1: Sprites" );

        for (uint32_t i = 0; i < NUM_BLOBS; i++)
        {
            graphics_draw_sprite_trans(disp, (int32_t) blobs[i].x, (int32_t) blobs[i].y, brew_sprite);
        }

        // Ball
        graphics_draw_sprite_trans(disp, (int32_t) (ball.x - ball_sprite->width/2), (int32_t) (ball.y - ball_sprite->height/2), ball_sprite);


        // TODO Draw center
        graphics_draw_line_trans(disp, (int32_t) ball.x, (int32_t) ball.y, (int32_t) ball.x, (int32_t) ball.y, graphics_make_color(0,255,0,255));
        // TODO draw velocity from ball center ??
        graphics_draw_line_trans(disp, (int32_t) ball.x, (int32_t) ball.y, (int32_t) ball.x + ball.dx*3, (int32_t) ball.y + ball.dy*3, graphics_make_color(0,0,255,255));


        for (uint32_t i = 0; i < NUM_BLOBS; i++)
        {
            graphics_draw_sprite_trans(disp, (int32_t) blobs[i].x, (int32_t) blobs[i].y, brew_sprite);
            // TODO Draw bounding box
            graphics_draw_line_trans(disp, (int32_t) blobs[i].x, (int32_t) blobs[i].y, (int32_t) blobs[i].x + brew_sprite->width, (int32_t) blobs[i].y, graphics_make_color(0,255,0,255));
            graphics_draw_line_trans(disp, (int32_t) blobs[i].x + brew_sprite->width, (int32_t) blobs[i].y, (int32_t) blobs[i].x + brew_sprite->width, (int32_t) blobs[i].y + brew_sprite->height, graphics_make_color(0,255,0,255));
            graphics_draw_line_trans(disp, (int32_t) blobs[i].x + brew_sprite->width, (int32_t) blobs[i].y + brew_sprite->height, (int32_t) blobs[i].x, (int32_t) blobs[i].y + brew_sprite->height, graphics_make_color(0,255,0,255));
            graphics_draw_line_trans(disp, (int32_t) blobs[i].x, (int32_t) blobs[i].y + brew_sprite->height, (int32_t) blobs[i].x, (int32_t) blobs[i].y, graphics_make_color(0,255,0,255));
            // Draw collision vectors
            if (collisions[i].length >= 0) {
                uint32_t color = (collisions[i].normalized.x != 0 || collisions[i].normalized.y != 0) ? graphics_make_color(0,0,255,255) : graphics_make_color(127,127,127,255);
                graphics_draw_line_trans(disp, (int32_t) collisions[i].pos.x, (int32_t) collisions[i].pos.y, (int32_t) collisions[i].pos.x + collisions[i].dir.x, (int32_t) collisions[i].pos.y + collisions[i].dir.y, color);
            }
            // TODO draw line between player center and ball center
            graphics_draw_line_trans(disp, (int32_t) blobs[i].x + brew_sprite->width/2, (int32_t) blobs[i].y + brew_sprite->height/2, (int32_t) ball.x, (int32_t) ball.y, graphics_make_color(255,255,0,255));
            
            // TODO draw velocity from player center ??
            graphics_draw_line_trans(disp, (int32_t) blobs[i].x + brew_sprite->width/2, (int32_t) blobs[i].y + brew_sprite->height/2, (int32_t) blobs[i].x + brew_sprite->width/2 + blobs[i].dx*3, (int32_t) blobs[i].y + brew_sprite->height/2 + blobs[i].dy*3, graphics_make_color(0,0,255,255));
        }

        // TODO draw net
        graphics_draw_sprite_trans(disp, (int32_t) net.x, (int32_t) net.y, net_sprite);
        graphics_draw_line_trans(disp, (int32_t) net.x, (int32_t) net.y, (int32_t) net.x + net_sprite->width, (int32_t) net.y, graphics_make_color(0,255,0,255));
        graphics_draw_line_trans(disp, (int32_t) net.x + net_sprite->width, (int32_t) net.y, (int32_t) net.x + net_sprite->width, (int32_t) net.y + net_sprite->height, graphics_make_color(0,255,0,255));
        graphics_draw_line_trans(disp, (int32_t) net.x + net_sprite->width, (int32_t) net.y + net_sprite->height, (int32_t) net.x, (int32_t) net.y + net_sprite->height, graphics_make_color(0,255,0,255));
        graphics_draw_line_trans(disp, (int32_t) net.x, (int32_t) net.y + net_sprite->height, (int32_t) net.x, (int32_t) net.y, graphics_make_color(0,255,0,255));

        // TODO Draw scores
        char scores[15];
        snprintf(scores, sizeof(scores), "Score: %d | %d", scorePlayer1, scorePlayer2);
        graphics_draw_text(disp, display_get_width()/4.0f, 40, scores);

        // TODO Draw debug data
        char debug[15];
        snprintf(debug, sizeof(debug), "Hits: %d (P%d)", hitCount, lastPlayer);
        graphics_draw_text(disp, 3.0*(display_get_width()/4.0f), 40, debug);
    } else {
        // TODO Can write to disp->buffer ?! --> memcpy ??? DMA ???
        static uint32_t offset;
        int len = TEX_FORMAT_PIX2BYTES(surface_get_format(disp), disp->width * disp->height) / 8;
        int lineLength = len / disp->height;
        uint64_t c64 = 0xffaa6600;  // 4 pixels @ 16bpp
        uint64_t *buffer = (uint64_t *)(disp->buffer);
        for( int i = 0; i < len; i++ ) {
            int line = (i / lineLength);
            if (line == offset) {
                buffer[i] = c64;
            } else if (line % 10 == 0) {
                buffer[i] = 0xffffffff;
            } else {
                buffer[i] = 0;
            }
        }
        offset = (offset+1) % disp->height;
    }

    /* Force backbuffer flip */
    display_show(disp);
}

#include <float.h>
#include "n64sys.h"

int main()
{
    debug_init_isviewer();
    debug_init_usblog();

    display_init(RESOLUTION_640x480, DEPTH_16_BPP, 3, GAMMA_NONE, ANTIALIAS_RESAMPLE);

    controller_init();
    timer_init();

    uint32_t display_width = display_get_width();
    uint32_t display_height = display_get_height();
    
    dfs_init(DFS_DEFAULT_LOCATION);

    rdpq_init();
    rdpq_debug_start();

    fprintf(stderr, "Starting\n");

    brew_sprite = sprite_load("rom:/n64brew.sprite");

    obj_min_x = 5;
    obj_max_x = display_width - 5;
    obj_min_y = 5;
    obj_max_y = display_height - 15;

    for (uint32_t i = 0; i < NUM_BLOBS; i++)
    {
        fprintf(stderr, "init blob[%ld]\n", i);
        object_t *obj = &blobs[i];

        obj->x = i == 0 ? 40 : display_width - brew_sprite->width - 40;
        obj->y = 200;
        obj->dx = 0;
        obj->dy = 0;
        obj->scale_factor = 1.0f;
        fprintf(stderr, "blob[%ld]: x=%f y=%f dx=%f dy=%f\n", i, obj->x, obj->y, obj->dx, obj->dy);
    }


    ball_sprite = sprite_load("rom:/ball.sprite");
    ball.x = display_width / 4.0f;
    ball.y = 0;
    ball.dx = 0;
    ball.dy = 0;
    ball.scale_factor = 1.0f;

    net_sprite = sprite_load("rom:/net.sprite");
    net.x = display_width/2.0f - (net_sprite->width/2.0f);
    net.y = display_height - net_sprite->height;
    net.dx = 0;
    net.dy = 0;
    net.scale_factor = 1.0f;

    tiles_sprite = sprite_load("rom:/tiles.sprite");

    surface_t tiles_surf = sprite_get_pixels(tiles_sprite);

    // Create a block for the background, so that we can replay it later.
    rspq_block_begin();

    // Check if the sprite was compiled with a paletted format. Normally
    // we should know this beforehand, but for this demo we pretend we don't
    // know. This also shows how rdpq can transparently work in both modes.
    bool tlut = false;
    tex_format_t tiles_format = sprite_get_format(tiles_sprite);
    if (tiles_format == FMT_CI4 || tiles_format == FMT_CI8) {
        // If the sprite is paletted, turn on palette mode and load the
        // palette in TMEM. We use the mode stack for demonstration,
        // so that we show how a block can temporarily change the current
        // render mode, and then restore it at the end.
        rdpq_mode_push();
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_tex_upload_tlut(sprite_get_palette(tiles_sprite), 0, 16);
        tlut = true;
    }
    uint32_t tile_width = tiles_sprite->width / tiles_sprite->hslices;
    uint32_t tile_height = tiles_sprite->height / tiles_sprite->vslices;
 
    for (uint32_t ty = 0; ty < display_height; ty += tile_height)
    {
        for (uint32_t tx = 0; tx < display_width; tx += tile_width)
        {
            // Load a random tile among the 4 available in the texture,
            // and draw it as a rectangle.
            // Notice that this code is agnostic to both the texture format
            // and the render mode (standard vs copy), it will work either way.
            int s = RANDN(2)*32, t = RANDN(2)*32;
            rdpq_tex_upload_sub(TILE0, &tiles_surf, NULL, s, t, s+32, t+32);
            rdpq_texture_rectangle(TILE0, tx, ty, tx+32, ty+32, s, t);
        }
    }
    
    // Pop the mode stack if we pushed it before
    if (tlut) rdpq_mode_pop();
    tiles_block = rspq_block_end();

    update(0);
    new_timer(TIMER_TICKS(1000000 / FRAMERATE), TF_CONTINUOUS, update);

    fprintf(stderr, "Entering main loop\n");

    int cur_frame = 0;
    while (1)
    {
        render(cur_frame, mode);

        controller_scan();
        struct controller_data ckeys = get_keys_down();
        struct controller_data pressed = get_keys_pressed();

        // TODO switch render mode rdpq / sprites ??
        if (ckeys.c[0].Z) {
            mode = (mode + 1) % 3;
        }

        for (uint32_t i = 0; i < NUM_BLOBS; i++)
        {
            object_t *obj = &blobs[i];
            if ((pressed.c[i].up || pressed.c[i].A || pressed.c[i].B) && (obj_max_y - fabs(obj->y) - brew_sprite->height) < POSITION_EPSILON) {
                obj->dy = -6;
            }

            if (pressed.c[i].left) {
                obj->dx = -3;
            }

            if (pressed.c[i].right) {
                obj->dx = 3;
            }

            if (fabs(pressed.c[i].x) > 5) {
                obj->dx = (pressed.c[i].x / 30);
            }
        }

        cur_frame++;
    }
}
