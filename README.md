
# Very minimal car player for Raspberry Pi

1. Minimal set of controls: only (two) GPIO buttons to choose genre of song.

2. The more often the user listens to the song, the more often it is played.


## Install

1. Make a [hardware](zenplay.kicad/export.pdf). Each button will choose a genre (see [settings.h](settings.h)).

1. apt install \  
  libhiredis-dev \  
  librhash-dev \  
  libmpv-dev \  
  libgpiod-dev \  

2. apt install redis \  
  && service redis start  

3. git clone https://github.com/sagb/zenplay.git \  
  && cd zenplay  

4. Edit settings.h

5. make \  
  && make install  

6. zenindex  _(index all songs once)_

7. zenplay

