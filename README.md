
# Very minimal car player for Raspberry Pi

1. Minimal set of controls: GPIO buttons to choose genre of song,
plus optional pause button

2. `popular_mode` constant in settings.h selects one of the following modes:
* 0: Random
* 1: The more often the user listens to the song, the more often it is played

3. `order_instead_of_shuffle` constant selects a submode of `popular_mode=0`:
* 0: Shuffle
* 1: Alphabetically order


## Install

1. Make a [hardware](zenplay.kicad/export.pdf). Each button will choose a genre,
plus an optional button for pause (see [settings.h](settings.h)).

2. apt install \  
  libhiredis-dev \  
  librhash-dev \  
  libmpv-dev \  
  libgpiod-dev \  

3. apt install redis \  
  && service redis start  

4. git clone https://github.com/sagb/zenplay.git \  
  && cd zenplay  

5. Edit settings.h

6. make \  
  && make install  

7. zenindex  _(index all songs once)_  
   _-or-_  
   zenindex [-p] dir1 dir2 ...  
   -p: purge redis tables before index

8. zenplay _(play random file and learn listen duration)_  
   _-or-_  
   zenplay file1 file2 ... _(debug mode, database not used)_

