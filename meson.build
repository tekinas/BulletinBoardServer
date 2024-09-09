project('Bulletin Board Server','cpp', default_options : ['cpp_std=c++23','warning_level=3'])
threads = dependency('threads')
executable('bbserv', './src/server.cpp', dependencies : threads)
