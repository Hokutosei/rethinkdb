#ifndef __DATA_PROVIDER_HPP__
#define __DATA_PROVIDER_HPP__

#include <boost/scoped_array.hpp>
#include <boost/scoped_ptr.hpp>
#include <vector>
#include <exception>
#include "errors.hpp"

#include "concurrency/cond_var.hpp"

class const_buffer_group_t {
public:
    struct buffer_t {
        ssize_t size;
        const void *data;
    };
    void add_buffer(size_t s, const void *d) {
        buffer_t b;
        b.size = s;
        b.data = d;
        buffers_.push_back(b);
    }
    size_t num_buffers() const {
        return buffers_.size();
    }
    buffer_t get_buffer(size_t i) const {
        return buffers_[i];
    }
    size_t get_size() const {
        size_t s = 0;
        for (int i = 0; i < (int)buffers_.size(); i++) {
            s += buffers_[i].size;
        }
        return s;
    }
private:
    std::vector<buffer_t> buffers_;
};

class buffer_group_t {
public:
    struct buffer_t {
        ssize_t size;
        void *data;
    };
    void add_buffer(size_t s, void *d) { inner_.add_buffer(s, d); }
    size_t num_buffers() const { return inner_.num_buffers(); }
    buffer_t get_buffer(size_t i) const {
        buffer_t ret;
        const_buffer_group_t::buffer_t tmp = inner_.get_buffer(i);
        ret.size = tmp.size;
        ret.data = const_cast<void *>(tmp.data);
        return ret;
    }
    size_t get_size() const { return inner_.get_size(); }
    friend const const_buffer_group_t *const_view(const buffer_group_t *group);

private:
    const_buffer_group_t inner_;
};

inline const const_buffer_group_t *const_view(const buffer_group_t *group) {
    return &group->inner_;
}


/* Data providers can throw data_provider_failed_exc_t to cancel the operation they are being used
for. In general no information can be carried along with the data_provider_failed_exc_t; it's meant
to signal to the data provider consumer, not the data provider creator. The cause of the error
should be communicated some other way. */

class data_provider_failed_exc_t : public std::exception {
    const char *what() {
        return "Data provider failed.";
    }
};

/* A data_provider_t conceptually represents a read-only array of bytes. It is an abstract
superclass; its concrete subclasses represent different sources of bytes.

In general, the data on a data_provider_t can only be requested once: once get_data_*() or discard()
has been called, they cannot be called again. This is to make it easier to implement data providers
that read off a socket or other one-time-use source of data. Note that it's not mandatory to read
the data at all--if a data provider really needs its data to be read, it must do it itself in the
destructor. */

class data_provider_t {
public:
    virtual ~data_provider_t() { }

    /* Consumers can call get_size() to figure out how many bytes long the byte array is. Producers
    should override get_size(). */
    virtual size_t get_size() const = 0;

    /* Consumers can call get_data_into_buffers() to ask the data_provider_t to fill a set of
    buffers that are provided. Producers should override get_data_into_buffers(). Alternatively,
    subclass from auto_copying_data_provider_t to get this behavior automatically in terms of
    get_data_as_buffers(). */
    virtual void get_data_into_buffers(const buffer_group_t *dest) throw (data_provider_failed_exc_t) = 0;

    /* Consumers can call get_data_as_buffers() to ask the data_provider_t to provide a set of
    buffers that already contain the data. The reason for this alternative interface is that some
    data providers already have the data in buffers, so this is more efficient than doing an extra
    copy. The buffers are guaranteed to remain valid until the data provider is destroyed. Producers
    should also override get_data_as_buffers(), or subclass from auto_buffering_data_provider_t to
    automatically implement it in terms of get_data_into_buffers(). */
    virtual const const_buffer_group_t *get_data_as_buffers() throw (data_provider_failed_exc_t) = 0;
};

/* A auto_buffering_data_provider_t is a subclass of data_provider_t that provides an implementation
of get_data_as_buffers() in terms of get_data_into_buffers(). It is itself an abstract class;
subclasses should override get_size() and get_data_into_buffers(). */

class auto_buffering_data_provider_t : public data_provider_t {
public:
    const const_buffer_group_t *get_data_as_buffers() throw (data_provider_failed_exc_t);
private:
    boost::scoped_array<char> buffer;   /* This is NULL until buffers are requested */
    const_buffer_group_t buffer_group;
};

/* A auto_copying_data_provider_t is a subclass of data_provider_t that implements
get_data_into_buffers() in terms of get_data_as_buffers(). It is itself an abstract class;
subclasses should override get_size(), get_data_as_buffers(), and done_with_buffers(). */

class auto_copying_data_provider_t : public data_provider_t {
public:
    void get_data_into_buffers(const buffer_group_t *dest) throw (data_provider_failed_exc_t);
};

/* A buffered_data_provider_t is a data_provider_t that simply owns an internal buffer that it
provides the data from. */

class buffered_data_provider_t : public auto_copying_data_provider_t {
public:
    explicit buffered_data_provider_t(data_provider_t *dp);   // Create with contents of another
    buffered_data_provider_t(const void *, size_t);   // Create by copying out of a buffer
    buffered_data_provider_t(size_t, void **);    // Allocate buffer, let creator fill it
    size_t get_size() const;
    const const_buffer_group_t *get_data_as_buffers() throw (data_provider_failed_exc_t);
private:
    size_t size;
    const_buffer_group_t bg;
    boost::scoped_array<char> buffer;
};

/* maybe_buffered_data_provider_t wraps another data_provider_t. It acts exactly like the
data_provider_t it wraps, even down to throwing the same exceptions in the same places. Internally,
it buffers the other data_provider_t if it is sufficiently small, improving performance. */

class maybe_buffered_data_provider_t : public data_provider_t {
public:
    maybe_buffered_data_provider_t(data_provider_t *dp, int threshold);

    size_t get_size() const;
    void get_data_into_buffers(const buffer_group_t *dest) throw (data_provider_failed_exc_t);
    const const_buffer_group_t *get_data_as_buffers() throw (data_provider_failed_exc_t);

private:
    int size;
    data_provider_t *original;
    // true if we decide to buffer but there is an exception. We catch the exception in the
    // constructor and then set this variable to true, then throw data_provider_failed_exc_t()
    // when our data is requested. This way we behave exactly the same whether or not we buffer.
    bool exception_was_thrown;
    boost::scoped_ptr<buffered_data_provider_t> buffer;   // NULL if we decide not to buffer
};


class buffer_borrowing_data_provider_t : public data_provider_t {
public:
    class side_data_provider_t : public auto_copying_data_provider_t {
    public:
        // Takes the thread that the reader reads from.  Soon after
        // construction, this serves as the de facto home thread of
        // the side_data_provider_t.
        side_data_provider_t(int reading_thread, size_t size);
        ~side_data_provider_t();

        size_t get_size() const;
        const const_buffer_group_t *get_data_as_buffers() throw (data_provider_failed_exc_t);
        void supply_buffers_and_wait(const buffer_group_t *buffers);

    private:
        int reading_thread_;
        unicond_t<const const_buffer_group_t *> cond_;
        cond_t done_cond_;
        size_t size_;
    };


    buffer_borrowing_data_provider_t(int side_reader_thread, data_provider_t *inner);
    ~buffer_borrowing_data_provider_t();
    size_t get_size() const;

    void get_data_into_buffers(const buffer_group_t *dest) throw (data_provider_failed_exc_t);

    const const_buffer_group_t *get_data_as_buffers() throw (data_provider_failed_exc_t);
    side_data_provider_t *side_provider();
private:
    data_provider_t *inner_;
    side_data_provider_t *side_;
    bool side_owned_;
};

/* Make a data_provider_splitter_t to send a single data provider to multiple locations. Call
branch() every time you want a separate data provider. All the data providers returned by branch()
will become invalid once the data_provider_splitter_t is destroyed. */

class data_provider_splitter_t {

public:
    data_provider_splitter_t(data_provider_t *dp);
    data_provider_t *branch();

private:
    struct reusable_provider_t : public auto_copying_data_provider_t {
        size_t size;
        const const_buffer_group_t *bg;   // NULL if exception should be thrown
        size_t get_size() const {
            return size;
        }
        const const_buffer_group_t *get_data_as_buffers() throw (data_provider_failed_exc_t) {
            if (bg) return bg;
            else throw data_provider_failed_exc_t();
        }
    } reusable_provider;
};

#endif /* __DATA_PROVIDER_HPP__ */