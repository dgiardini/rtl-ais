`rtl-ais`, a simple AIS tuner and generic dual-frequency FM demodulator
-----------------------------------------------------------------------

rtl-ais provides the `rtl_ais` command, which decodes AIS data from Software Defined Radio (SDR) and outputs `AIVDM` / `AIVDO` sentences.

| OS support |   |
|------------|---|
| Linux      | ✅ |
| Windows    | ✅ |
| OSX        | ✅ |


Command Line
------------
```
Use: rtl_ais [options] [outputfile]
        [-l left_frequency (default: 161.975M)]
        [-r right_frequency (default: 162.025M)]
            left freq < right freq
            frequencies must be within 1.2MHz
        [-s sample_rate (default: 24k)]
            maximum value, might be down to 12k
        [-o output_rate (default: 48k)]
            must be equal or greater than twice -s value
        [-E toggle edge tuning (default: off)]
        [-D toggle DC filter (default: on)]
        [-d device_index (default: 0)]
        [-g tuner_gain (default: automatic)]
        [-p ppm_error (default: 0)]
        [-R enable RTL chip AGC (default: off)]
        [-A turn off built-in AIS decoder (default: on)]
            use this option to output samples to file or stdout.
        Built-in AIS decoder options:
        [-h host (default: 127.0.0.1)]
        [-P port (default: 10110)]
        [-T use TCP communication as tcp listener ( -h is ignored)]
        [-t time to keep ais messages in sec, using tcp listener (default: 15)]
        [-n log NMEA sentences to console (stderr) (default off)]
        [-L log sound levels to console (stderr) (default off)]
        [-I add sample index to NMEA mesages (default off)]
        [-S seconds_for_decoder_stats (default 0=off)]
        When the built-in AIS decoder is disabled the samples are sent to
        to [outputfile] (a '-' dumps samples to stdout)
            omitting the filename also uses stdout
        Output is stereo 2x16 bit signed ints
        Examples:
        Receive AIS traffic,sent UDP NMEA sentences to 127.0.0.1 port 10110
             and log the senteces to console:
        rtl_ais -n
        Tune two fm stations and play one on each channel:
        rtl_ais -l233.15M  -r233.20M -A  | play -r48k -traw -es -b16 -c2 -V1 -
```


Compiling
---------
Make sure you have the following dependencies:
  - librtlsdr
  - libusb
  - libpthread

```console
$ # Get the source code:
$ git clone https://github.com/dgiardini/rtl-ais
$ # Change to the source dir
$ cd rtl-ais
$ make
$ # Test running the command
$ ./rtl_ais
```

For compiling a MS Windows executable you will need a working MSYS/MinGW environment.
Edit the `Makefile`, and modify these lines:

```Makefile
#### point this to your correct path ###
RTLSDR_PATH="/c/tmp/rtl-sdr/"
RTLSDR_LIB=$(RTLSDR_PATH)/build/src/
########################################
```


Installing
----------
* On Linux, `sudo make install`
* On Windows, put the `librtlsdr.dll` and `libusb-1.0.dll` files in the same directory
with `rtl_ais.exe`. You'll need the `zadig` driver installed too.


Running
-------

rtl-ais uses software defined radio (SDR).  The specific
hardware we use for this is a DVB-T dongle. A good starting point is:
https://www.rtl-sdr.com/about-rtl-sdr

You'll need also an antenna, and be located near (some miles)  the
passing vessels.

You'll also need to do some procedure to get the tunning error for the
specfic dongle you have (aka ppm error), and pass that number as parameter
of rtl-ais.


Docker Container
----------------
Now you can run rtl-ais in a docker container. No dependencies to install. Total container size is 55 to 75 MB, depending on the host architecture. Get/install docker [here](https://docs.docker.com/get-docker/).

Two options for obtaining the container: Either download and run a pre-built container, or build the container locally.

 1. Just to test things out: `docker run -it --rm --device=/dev/bus/usb ghcr.io/bklofas/rtl-ais:latest`
    * This downloads a pre-built container from the Github container registry.
    * Architectures supported are i386, amd64, arm32v6, arm32v7, and arm64. Tested on amd64, arm32v7, and arm64. arm packages run on all RaspberryPi flavors. Your client will automatically download the appropriate architecture.
    * This image will run by default `./rtl_ais -n`, showing the received packets on STDOUT. Uses all other default values.
    * You can add other ./rtl-sdr options, see below.
    * Make sure at least one RTL-SDR dongle is connected.
    * Startup messages and decoded packets will display in the terminal.
    * Ctrl-C to kill.
    * Using the `--rm` flag will delete the container when you kill it. Otherwise, it will stay around until you prune.

 1. For a more permanent setup, run the container in the background and add any options you want: `docker run -d --name rtl-ais --restart=unless-stopped --log-driver=local --network=host --device=/dev/bus/usb ghcr.io/bklofas/rtl-ais:latest ./rtl_ais -n -d 00000002 -h 127.0.0.1 -P 10110`
    * -d: Start this container in daemon/background mode.
    * --name: Name this anything you want.
    * --restart=unless-stopped: Automatically restart the container if something happens (reboot, USB problem), unless you have manually stopped the container.
    * --log-driver=local: By default, docker uses the json log driver which may fill up your harddrive, depending on how busy your station is. `local` log driver defaults to 100MB of saved logs, and automatically rotates them.
    * --network=host: Allows the container to talk to the internet, if you are sending the packets to an online service.
    * --device=: Allows the container to talk to the USB bus to access the RTL-SDR dongle.
    * ./rtl_ais: Same command-line options as non-containerized.
    * View the startup messages and decoded packets with `docker logs --follow rtl-ais`
    * Stop the container with `docker stop rtl-ais`

Building the container:

 * Clone the repository with `git clone https://github.com/dgiardini/rtl-ais.git` , then from the folder `docker build -t rtl-ais .`
 *  Or, build the container without cloning the repository: `docker build https://github.com/dgiardini/rtl-ais.git`

Other tips and tricks:

 * If you have the `-n` flag, view the decoded AIS packets in real-time with `docker logs --follow rtl-ais`
 * If you are only sending packets to one internet service (such as marinetraffic.com), you can use the `-h` and `-P` options that they provide.
 * You are encouraged to send your decoded packets to multiple online services. Check out AIS packet multiplexer [kplex](http://www.stripydog.com/kplex/), pre-built containers are available [here](https://ghcr.io/bklofas/kplex).


Testing
-------

TODO: something like
https://github.com/freerange/ais-on-sdr/wiki/Testing-AISDecoder#with-an-audio-file


Known Issues
------------
* The `[-p ppm error]` parameter is essential for rtl_ais to work.
  * The ppm error is the frequency deviation in parts per million from the desired tuning
frequency, and the real tuned frequency due to the crystal oscillator deviation. This
figure is different for each device, it's very important to know  this value and pass this parameter to rtl_ais.
  * Some instructions for get the ppm error are here:
    http://www.rtl-sdr.com/how-to-calibrate-rtl-sdr-using-kalibrate-rtl-on-linux
  * and here (using SDR#):
    http://www.atouk.com/SDRSharpQuickStart.html#adjusting
  * and here (using HDSDR ad AIS traffic)
    http://www.cruisersforum.com/forums/f134/new-rtlsdr-plugin-102929-11.html#post1844966
