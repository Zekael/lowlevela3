#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <linux/input.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <fcntl.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/mman.h>


//definitions
#define fb_path "/dev/fb%d"
#define fb_id "RPi-Sense FB"
//the given id didnt work so i found it manually
#define event_id_joystick "Raspberry Pi Sense HAT Joystick" //The value the was given to me is not what i found when i checked the event id
#define event_path "/dev/input/event%d"
#define poll_timeout 0


// The game state can be used to detect what happens on the playfield
#define GAMEOVER   0
#define ACTIVE     (1 << 0)
#define ROW_CLEAR  (1 << 1)
#define TILE_ADDED (1 << 2)

// If you extend this structure, either avoid pointers or adjust
// the game logic allocate/deallocate and reset the memory
typedef struct {
  bool occupied;
} tile;

typedef struct {
  unsigned int x;
  unsigned int y;
} coord;

typedef struct {
  coord const grid;                     // playfield bounds
  unsigned long const uSecTickTime;     // tick rate
  unsigned long const rowsPerLevel;     // speed up after clearing rows
  unsigned long const initNextGameTick; // initial value of nextGameTick

  unsigned int tiles; // number of tiles played
  unsigned int rows;  // number of rows cleared
  unsigned int score; // game score
  unsigned int level; // game level

  tile *rawPlayfield; // pointer to raw memory of the playfield
  tile **playfield;   // This is the play field array
  unsigned int state;
  coord activeTile;                       // current tile

  unsigned long tick;         // incremeted at tickrate, wraps at nextGameTick
                              // when reached 0, next game state calculated
  unsigned long nextGameTick; // sets when tick is wrapping back to zero
                              // lowers with increasing level, never reaches 0
} gameConfig;



gameConfig game = {
                   .grid = {8, 8},
                   .uSecTickTime = 10000,
                   .rowsPerLevel = 2,
                   .initNextGameTick = 50,
};

//storing data about frame buffer and event location
typedef struct {
  int fb_fb;
  char* fb_name;
  int event_eb;
  char* event_name;
  struct fb_var_screeninfo vinfo;
  struct fb_fix_screeninfo finfo;
  char* fb_mem;
} initializeSenseHatVals;

initializeSenseHatVals initSenseHat = {.fb_fb = -1, .event_eb = -1, .vinfo = {0}, .finfo = {0}, .event_name = NULL, .fb_name = NULL, .fb_mem = NULL};

typedef struct {
  uint16_t color;
} color_tile;

color_tile set_color(uint16_t r, uint16_t g, uint16_t b) {

  color_tile color {0};

  //shift so the lower bits are in the right spot
  r = r << 11;
  g = g << 5;
  //and with a "mask"
  uint16_t green_cut = g & 0xF800;
  uint16_t red_cut = r & 0x07E0;
  uint16_t blue_cut = b & 0x001F;

  color.color = color + red_cut;
  color.color = color + green_cut;
  color.color = color + blue_cut;
  return color;
}

// This function is called on the start of your application
// Here you can initialize what ever you need for your task
// return false if something fails, else true
bool initializeSenseHat() {
  //variables
  struct fb_fix_screeninfo statInfo;
  struct fb_var_screeninfo varInfo;

  bool success = true;

  int end = 0;
  int i = 0;
  int fb = 0;
  char buff[30];

  //loop all versions of the fb + i
  while(end!=1){

    snprintf(buff, 30, fb_path, i);
    fb = open(buff, O_RDWR);

    //fb not found, asuming incremntal naming this means we didnt find it so return error
    if (fb == -1) {
      printf("Error in framebuffer device not found\n");
      success = false;
      end = 1;
    }
    //read fixed screen data
    if(ioctl(fb, FBIOGET_FSCREENINFO, &statInfo) == -1) {
      printf("ioctl failed fixed\n");
      success = false;
      end = 1;
    }
    //read variable screen data
    if(ioctl(fb, FBIOGET_VSCREENINFO, &varInfo) == -1) {
      printf("ioctl failed var\n");
      success = false;
      end = 1;
    }
    //check if matching id
    if(strcmp(statInfo.id, fb_id) == 0){
      end = 1;
      printf("Framebuffer found, %s\n",statInfo.id);
      break;
    }
    //increment
    else{
      printf("Framebuffer not found\n");
      printf("abandoned id: %s\n", statInfo.id);
      i++;
      close(fb);
    }
  }

  //store values
  initSenseHat.fb_fb = fb;
  initSenseHat.finfo = statInfo;
  initSenseHat.vinfo = varInfo;
  initSenseHat.fb_name = malloc(sizeof(char)*30);
  memccpy(initSenseHat.fb_name, buff, 0, 30);
  //map framebuffer
  void* fb_mem = mmap(NULL, statInfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
  initSenseHat.fb_mem = fb_mem;

  if(fb_mem == (void *)-1){
    printf("Error in mmap\n");
    success = false;
  }

  //found valid fb
  printf("id %s\n", statInfo.id);


  //find event device
  struct input_event event;
  struct pollfd fds[1];

  //variables
  end = 0;
  int eb = 0;
  i = 0;
  char buffer[30];

  //loop all versions of the fb + i
  while(end!=1){

    snprintf(buffer, 30, event_path, i);
    eb = open(buffer, O_RDWR | O_NONBLOCK);

    //fd not found, asuming incremntal naming this means we didnt find it so return error
    if (eb == -1) {
      printf("Error in framebuffer device not found\n");
      success = false;
      end = 1;
    }

    //get name
    char id_name[200] = "c string";
    if(ioctl(eb, EVIOCGNAME(sizeof(id_name)), id_name) == -1) {
      printf("ioctl failed\n");
      success = false;
      end = 1;
    }

    //check if matching id
    if(strcmp(id_name, event_id_joystick) == 0){
      end = 1;
    }else{
      //printf("id %s, did not match\n", id_name);
      close(eb);
      i++;
      continue;
    }
  }

  //store values
  initSenseHat.event_eb = eb;
  initSenseHat.event_name = malloc(sizeof(char)*30);
  memccpy(initSenseHat.event_name, buffer, 0, 30);

  return true;
}

// This function is called when the application exits
// Here you can free up everything that you might have opened/allocated
void freeSenseHat() {
  free(initSenseHat.event_name);
  free(initSenseHat.fb_name);
  close(initSenseHat.fb_fb);
  close(initSenseHat.event_eb);
  munmap(initSenseHat.fb_mem, initSenseHat.finfo.smem_len);
}

// This function should return the key that corresponds to the joystick press
// KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, with the respective direction
// and KEY_ENTER, when the the joystick is pressed
// !!! when nothing was pressed you MUST return 0 !!!
int readSenseHatJoystick() {

  struct input_event event;
  struct pollfd fds[1];
  char buff[30];
  int eb = initSenseHat.event_eb;
  
  fds[0].fd = eb;
  fds[0].events = POLLIN;

  poll(fds, 1, poll_timeout);

  //check if event data in
  if(fds[0].revents & POLLIN) {
    read(eb, &event, sizeof(event));
    /* printf("Event Type - %d\n", event.type);
    printf("Event Value - %d\n", event.value);
    printf("Event Code - %d\n", event.code); */
    if(event.type == EV_KEY && event.value == 1 || event.value == 2) {
      if (event.code == 103) {
        return KEY_UP;
      }
      else if (event.code == 108) {
        return KEY_DOWN;
      }
      else if (event.code == 105) {
        return KEY_LEFT;
      }
      else if (event.code == 106) {
        return KEY_RIGHT;
      }
      else if (event.code == 28) {
        return KEY_ENTER;
      }
      else {
        return 0;
      }
    }
  }
  return 0;
}


// This function should render the gamefield on the LED matrix. It is called
// every game tick. The parameter playfieldChanged signals whether the game logic
// has changed the playfield
void renderSenseHatMatrix(bool const playfieldChanged) {
  if(playfieldChanged){
    //variables
    int fb = initSenseHat.fb_fb;
    void* fb_mem = initSenseHat.fb_mem;
    uint16_t** color_field = (uint16_t**)fb_mem;
    tile** playfield = game.playfield;
    
    int width = game.grid.x;
    int height = game.height.y;

    for (size_t i = 0; i < width; i++)
    {
      for (size_t j = 0; j < height; j++)
      {
        
        bool occupied = playfield[i][j];
        if(occupied){
          color_field[i][j] = 0xFFFF;
        }else{
          color_field[i][j] = 0x0000;
        }
      }
    }
  }
}


// The game logic uses only the following functions to interact with the playfield.
// if you choose to change the playfield or the tile structure, you might need to
// adjust this game logic <> playfield interface

static inline void newTile(coord const target) {
  game.playfield[target.y][target.x].occupied = true;
}

static inline void copyTile(coord const to, coord const from) {
  memcpy((void *) &game.playfield[to.y][to.x], (void *) &game.playfield[from.y][from.x], sizeof(tile));
}

static inline void copyRow(unsigned int const to, unsigned int const from) {
  memcpy((void *) &game.playfield[to][0], (void *) &game.playfield[from][0], sizeof(tile) * game.grid.x);

}

static inline void resetTile(coord const target) {
  memset((void *) &game.playfield[target.y][target.x], 0, sizeof(tile));
}

static inline void resetRow(unsigned int const target) {
  memset((void *) &game.playfield[target][0], 0, sizeof(tile) * game.grid.x);
}

static inline bool tileOccupied(coord const target) {
  return game.playfield[target.y][target.x].occupied;
}

static inline bool rowOccupied(unsigned int const target) {
  for (unsigned int x = 0; x < game.grid.x; x++) {
    coord const checkTile = {x, target};
    if (!tileOccupied(checkTile)) {
      return false;
    }
  }
  return true;
}


static inline void resetPlayfield() {
  for (unsigned int y = 0; y < game.grid.y; y++) {
    resetRow(y);
  }
}

// Below here comes the game logic. Keep in mind: You are not allowed to change how the game works!
// that means no changes are necessary below this line! And if you choose to change something
// keep it compatible with what was provided to you!

bool addNewTile() {
  game.activeTile.y = 0;
  game.activeTile.x = (game.grid.x - 1) / 2;
  if (tileOccupied(game.activeTile))
    return false;
  newTile(game.activeTile);
  return true;
}

bool moveRight() {
  coord const newTile = {game.activeTile.x + 1, game.activeTile.y};
  if (game.activeTile.x < (game.grid.x - 1) && !tileOccupied(newTile)) {
    copyTile(newTile, game.activeTile);
    resetTile(game.activeTile);
    game.activeTile = newTile;
    return true;
  }
  return false;
}

bool moveLeft() {
  coord const newTile = {game.activeTile.x - 1, game.activeTile.y};
  if (game.activeTile.x > 0 && !tileOccupied(newTile)) {
    copyTile(newTile, game.activeTile);
    resetTile(game.activeTile);
    game.activeTile = newTile;
    return true;
  }
  return false;
}


bool moveDown() {
  coord const newTile = {game.activeTile.x, game.activeTile.y + 1};
  if (game.activeTile.y < (game.grid.y - 1) && !tileOccupied(newTile)) {
    copyTile(newTile, game.activeTile);
    resetTile(game.activeTile);
    game.activeTile = newTile;
    return true;
  }
  return false;
}


bool clearRow() {
  if (rowOccupied(game.grid.y - 1)) {
    for (unsigned int y = game.grid.y - 1; y > 0; y--) {
      copyRow(y, y - 1);
    }
    resetRow(0);
    return true;
  }
  return false;
}

void advanceLevel() {
  game.level++;
  switch(game.nextGameTick) {
  case 1:
    break;
  case 2 ... 10:
    game.nextGameTick--;
    break;
  case 11 ... 20:
    game.nextGameTick -= 2;
    break;
  default:
    game.nextGameTick -= 10;
  }
}

void newGame() {
  game.state = ACTIVE;
  game.tiles = 0;
  game.rows = 0;
  game.score = 0;
  game.tick = 0;
  game.level = 0;
  resetPlayfield();
}

void gameOver() {
  game.state = GAMEOVER;
  game.nextGameTick = game.initNextGameTick;
}


bool sTetris(int const key) {
  bool playfieldChanged = false;

  if (game.state & ACTIVE) {
    // Move the current tile
    if (key) {
      playfieldChanged = true;
      switch(key) {
      case KEY_LEFT:
        moveLeft();
        break;
      case KEY_RIGHT:
        moveRight();
        break;
      case KEY_DOWN:
        while (moveDown()) {};
        game.tick = 0;
        break;
      default:
        playfieldChanged = false;
      }
    }

    // If we have reached a tick to update the game
    if (game.tick == 0) {
      // We communicate the row clear and tile add over the game state
      // clear these bits if they were set before
      game.state &= ~(ROW_CLEAR | TILE_ADDED);

      playfieldChanged = true;
      // Clear row if possible
      if (clearRow()) {
        game.state |= ROW_CLEAR;
        game.rows++;
        game.score += game.level + 1;
        if ((game.rows % game.rowsPerLevel) == 0) {
          advanceLevel();
        }
      }

      // if there is no current tile or we cannot move it down,
      // add a new one. If not possible, game over.
      if (!tileOccupied(game.activeTile) || !moveDown()) {
        if (addNewTile()) {
          game.state |= TILE_ADDED;
          game.tiles++;
        } else {
          gameOver();
        }
      }
    }
  }

  // Press any key to start a new game
  if ((game.state == GAMEOVER) && key) {
    playfieldChanged = true;
    newGame();
    addNewTile();
    game.state |= TILE_ADDED;
    game.tiles++;
  }

  return playfieldChanged;
}

int readKeyboard() {
  struct pollfd pollStdin = {
       .fd = STDIN_FILENO,
       .events = POLLIN
  };
  int lkey = 0;

  if (poll(&pollStdin, 1, 0)) {
    lkey = fgetc(stdin);
    if (lkey != 27)
      goto exit;
    lkey = fgetc(stdin);
    if (lkey != 91)
      goto exit;
    lkey = fgetc(stdin);
  }
 exit:
    switch (lkey) {
      case 10: return KEY_ENTER;
      case 65: return KEY_UP;
      case 66: return KEY_DOWN;
      case 67: return KEY_RIGHT;
      case 68: return KEY_LEFT;
    }
  return 0;
}

void renderConsole(bool const playfieldChanged) {
  if (!playfieldChanged)
    return;

  // Goto beginning of console
  fprintf(stdout, "\033[%d;%dH", 0, 0);
  for (unsigned int x = 0; x < game.grid.x + 2; x ++) {
    fprintf(stdout, "-");
  }
  fprintf(stdout, "\n");
  for (unsigned int y = 0; y < game.grid.y; y++) {
    fprintf(stdout, "|");
    for (unsigned int x = 0; x < game.grid.x; x++) {
      coord const checkTile = {x, y};
      fprintf(stdout, "%c", (tileOccupied(checkTile)) ? '#' : ' ');
    }
    switch (y) {
      case 0:
        fprintf(stdout, "| Tiles: %10u\n", game.tiles);
        break;
      case 1:
        fprintf(stdout, "| Rows:  %10u\n", game.rows);
        break;
      case 2:
        fprintf(stdout, "| Score: %10u\n", game.score);
        break;
      case 4:
        fprintf(stdout, "| Level: %10u\n", game.level);
        break;
      case 7:
        fprintf(stdout, "| %17s\n", (game.state == GAMEOVER) ? "Game Over" : "");
        break;
    default:
        fprintf(stdout, "|\n");
    }
  }
  for (unsigned int x = 0; x < game.grid.x + 2; x++) {
    fprintf(stdout, "-");
  }
  fflush(stdout);
}


inline unsigned long uSecFromTimespec(struct timespec const ts) {
  return ((ts.tv_sec * 1000000) + (ts.tv_nsec / 1000));
}

int main(int argc, char **argv) {

  /* initializeSenseHat();

  while (1)
  {
    int received = readSenseHatJoystick();
    printf("%d\n", received);
  }
  

} */
  (void) argc;
  (void) argv;
  // This sets the stdin in a special state where each
  // keyboard press is directly flushed to the stdin and additionally
  // not outputted to the stdout
  {
    struct termios ttystate;
    tcgetattr(STDIN_FILENO, &ttystate);
    ttystate.c_lflag &= ~(ICANON | ECHO);
    ttystate.c_cc[VMIN] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &ttystate);
  }

  // Allocate the playing field structure
  game.rawPlayfield = (tile *) malloc(game.grid.x * game.grid.y * sizeof(tile));
  game.playfield = (tile**) malloc(game.grid.y * sizeof(tile *));
  if (!game.playfield || !game.rawPlayfield) {
    fprintf(stderr, "ERROR: could not allocate playfield\n");
    return 1;
  }
  for (unsigned int y = 0; y < game.grid.y; y++) {
    game.playfield[y] = &(game.rawPlayfield[y * game.grid.x]);
  }

  // Reset playfield to make it empty
  resetPlayfield();
  // Start with gameOver
  gameOver();

  if (!initializeSenseHat()) {
    fprintf(stderr, "ERROR: could not initilize sense hat\n");
    return 1;
  };

  // Clear console, render first time
  fprintf(stdout, "\033[H\033[J");
  renderConsole(true);
  renderSenseHatMatrix(true);

  while (true) {
    struct timeval sTv, eTv;
    gettimeofday(&sTv, NULL);

    int key = readSenseHatJoystick();
    //if (!key)
      //key = readKeyboard();
    if (key == KEY_ENTER)
      break;

    bool playfieldChanged = sTetris(key);
    renderConsole(playfieldChanged);
    renderSenseHatMatrix(playfieldChanged);

    // Wait for next tick
    gettimeofday(&eTv, NULL);
    unsigned long const uSecProcessTime = ((eTv.tv_sec * 1000000) + eTv.tv_usec) - ((sTv.tv_sec * 1000000 + sTv.tv_usec));
    if (uSecProcessTime < game.uSecTickTime) {
      usleep(game.uSecTickTime - uSecProcessTime);
    }
    game.tick = (game.tick + 1) % game.nextGameTick;
  }

  freeSenseHat();
  free(game.playfield);
  free(game.rawPlayfield);

  return 0;
}
