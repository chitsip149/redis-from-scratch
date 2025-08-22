package io_multiplexing

import (
	"IOMultiplexing/internal/config"
	"log"
	"syscall"
)

type Epoll struct {
	fd            int
	epollEvents   []syscall.EpollEvent
	genericEvents []Event
}

func createEvent(ep syscall.EpollEvent) Event {
	var op Operation = OpRead
	if ep.Events == syscall.EPOLLOUT {
		op = OpWrite
	}
	return Event{
		Fd: int(ep.Fd),
		Op: op,
	}
}

func (e Event) toNative() syscall.EpollEvent {
	var event uint32 = syscall.EPOLLIN
	if e.Op == OpWrite {
		event = syscall.EPOLLOUT
	}
	return syscall.EpollEvent{
		Fd:     int32(e.Fd),
		Events: event,
	}
}

func CreateIOMultiplexer() (*Epoll, error) {
	epollFD, err := syscall.EpollCreate1(0)
	if err != nil {
		log.Fatal(err)
		return nil, err
	}
	return &Epoll{
		fd:            epollFD,
		epollEvents:   make([]syscall.EpollEvent, config.MaxConnection),
		genericEvents: make([]Event, config.MaxConnection),
	}, nil
}

func (ep *Epoll) Monitor(event Event) error {
	epollEvents := event.toNative()

	//add event.Fd to the monitoring list of ep.fd
	return syscall.EpollCtl(ep.fd, syscall.EPOLL_CTL_ADD, event.Fd, &epollEvents)
}

func (ep *Epoll) Wait() ([]Event, error) {
	n, err := syscall.EpollWait(ep.fd, ep.epollEvents, -1)
	if err != nil {
		return nil, err
	}
	for i := 0; i < n; i++ {
		ep.genericEvents[i] = createEvent(ep.epollEvents[i])
	}
	return ep.genericEvents[:n], nil
}

func (ep *Epoll) Close() error {
	return syscall.Close(ep.fd)
}
