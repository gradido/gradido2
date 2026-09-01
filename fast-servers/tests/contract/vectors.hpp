/*
 * Reading a file out of `contracts/test-vectors/`, the half of it that is the same for every
 * subject.
 *
 * A contract vector file is read by two runners: this one and a TypeScript one in
 * `packages/contract-tests/`. Neither owns the file and neither may skip a vector it does not
 * like -- a vector nobody runs is a disagreement nobody reports, which is the failure the whole
 * arrangement exists to prevent. So the envelope is checked rather than trusted: the declared
 * `count` has to be the number of vectors actually read, and every id has to be unique and to
 * name its subject. `packages/contract-tests/src/vectors.ts` checks the same three things.
 *
 * What is *in* a vector is the subject's business; nothing here knows a field name.
 *
 * ### Why the field helpers are one walk per field
 *
 * arnm 0.7.5 reads an object by handing over a table, and a table converts a member where it
 * stands: a member of the wrong type -- `null` included -- stops the walk and leaves every entry
 * after it unset. A vector carries nullable fields, so a single table over one would stop at the
 * first `null` in it and read nothing beyond. The shape that works, and the one
 * `gradido-blockchain-core` uses for the same reason, is a table for what is always there and a
 * one-field walk for each member that may legitimately be absent. `nullableString()` is that
 * walk: absent, `null` and the wrong type are one answer, which is the reading a nullable field
 * wants.
 *
 * That costs a pass over the member chain per nullable field. A vector file is read once by a
 * test binary, so what it buys -- a field that reads as absent instead of silencing the four
 * fields behind it -- is worth more here than the walk.
 *
 * ### Where the file is
 *
 * `FS_CONTRACT_VECTORS_DIR` is compiled in by both builds and points at the checkout the binary
 * was built from. `GRADIDO_CONTRACT_VECTORS_DIR` overrides it, for a binary that was moved.
 */
#ifndef FS_TESTS_CONTRACT_VECTORS_HPP
#define FS_TESTS_CONTRACT_VECTORS_HPP

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include "arnm/json_reader.h"
}

#ifndef FS_CONTRACT_VECTORS_DIR
#error "FS_CONTRACT_VECTORS_DIR must be defined by the build; see build.zig and CMakeLists.txt"
#endif

namespace contract
{

/**
 * One subject's vector file, parsed and standing.
 *
 * The document lives as long as the object does and every `arnm_json_value *` handed out is a
 * view into it. Not copyable, because the reader holds allocator hooks that point into its own
 * storage.
 */
class VectorFile
{
  public:
    explicit VectorFile(const std::string &subject) : subject_(subject)
    {
        const std::string path = locate(subject);

        std::FILE *file = std::fopen(path.c_str(), "rb");
        if (file == nullptr)
            throw std::runtime_error("cannot open " + path +
                                     " (set GRADIDO_CONTRACT_VECTORS_DIR if the binary moved)");
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        if (size <= 0) {
            std::fclose(file);
            throw std::runtime_error(path + " is empty");
        }
        /* The insitu parse reads four bytes past the document and writes zeroes there, which is
         * what lets its scanner run without a bounds test in its inner loop. */
        text_.resize(static_cast<size_t>(size) + ARNM_JSON_READER_INSITU_PADDING, '\0');
        const size_t read = std::fread(text_.data(), 1, static_cast<size_t>(size), file);
        std::fclose(file);
        if (read != static_cast<size_t>(size))
            throw std::runtime_error("short read on " + path);

        /* The host allocator: a vector file is read once by a test binary, and an arena sized
         * for the largest of them would be one more number to keep in step with the files. */
        if (arnm_json_reader_init(&reader_, nullptr) != ARNM_SUCCESS)
            throw std::runtime_error("cannot initialise the json reader");
        initialised_ = true;

        arnm_json_value *root = nullptr;
        if (arnm_json_reader_parse_insitu(&reader_, text_.data(), static_cast<uint32_t>(size),
                                          static_cast<uint32_t>(text_.size()), false,
                                          &root) != ARNM_SUCCESS)
            throw std::runtime_error(path + ": " + arnm_json_reader_error_message(&reader_) +
                                     " at byte " +
                                     std::to_string(arnm_json_reader_error_position(&reader_)));

        read_envelope(path, root);
    }

    ~VectorFile()
    {
        if (initialised_)
            arnm_json_reader_release(&reader_);
    }

    VectorFile(const VectorFile &) = delete;
    VectorFile &operator=(const VectorFile &) = delete;

    /** The vectors, in the order the file writes them. */
    const std::vector<arnm_json_value *> &vectors() const { return vectors_; }

    /** The root object, for a subject that reads something beside `vectors` out of the file. */
    arnm_json_value *root() const { return root_; }

  private:
    static std::string locate(const std::string &subject)
    {
        const char *dir = std::getenv("GRADIDO_CONTRACT_VECTORS_DIR");
        return std::string(dir != nullptr ? dir : FS_CONTRACT_VECTORS_DIR) + "/" + subject +
               ".json";
    }

    void read_envelope(const std::string &path, arnm_json_value *root);

    std::string subject_;
    std::vector<char> text_;
    arnm_json_reader reader_ {};
    bool initialised_ = false;
    arnm_json_value *root_ = nullptr;
    std::vector<arnm_json_value *> vectors_;
};

/*
 * The field helpers. Each is one walk of @p object's member chain, stopping at the key it wants.
 */

/** @return the member as text, or nothing where it is absent, `null` or of another type */
inline std::optional<std::string> nullableString(arnm_json_value *object, const char *key)
{
    arnm_memory_block block {};
    arnm_json_field field {key, static_cast<uint32_t>(std::strlen(key)),
                           ARNM_JSON_FIELD_TYPE_STRING, &block};
    uint64_t found = 0;

    arnm_json_read_object(object, &field, 1, &found);
    if (found == 0 || block.data == nullptr)
        return std::nullopt;
    return std::string(reinterpret_cast<const char *>(block.data), block.size);
}

/** @throws std::runtime_error where the member is absent or is not a string */
inline std::string requiredString(arnm_json_value *object, const char *key)
{
    const std::optional<std::string> value = nullableString(object, key);
    if (!value.has_value())
        throw std::runtime_error(std::string("required member \"") + key + "\" is missing");
    return *value;
}

/** @throws std::runtime_error where the member is absent or is not `true`/`false` */
inline bool requiredBool(arnm_json_value *object, const char *key)
{
    bool value = false;
    arnm_json_field field {key, static_cast<uint32_t>(std::strlen(key)),
                           ARNM_JSON_FIELD_TYPE_BOOL, &value};
    uint64_t found = 0;

    arnm_json_read_object(object, &field, 1, &found);
    if (found == 0)
        throw std::runtime_error(std::string("required member \"") + key + "\" is not a bool");
    return value;
}

/**
 * A member that is a JSON number, which in `contracts/` is only ever a structural one.
 *
 * The envelope's `contractVersion` and `count` are the two: they are the file's own shape rather
 * than values it carries, they are small, and every existing contract file spells them this way.
 * Everything a vector *holds* is text -- see @ref requiredDecimal.
 *
 * @throws std::runtime_error where the member is absent or is not a number
 */
inline int64_t requiredNumber(arnm_json_value *object, const char *key)
{
    int64_t value = 0;
    arnm_json_field field {key, static_cast<uint32_t>(std::strlen(key)),
                           ARNM_JSON_FIELD_TYPE_INT64, &value};
    uint64_t found = 0;

    arnm_json_read_object(object, &field, 1, &found);
    if (found == 0)
        throw std::runtime_error(std::string("required member \"") + key + "\" is not a number");
    return value;
}

/**
 * A member the contract spells as a decimal string, read as the number it names.
 *
 * `contracts/AGENTS.md`: numbers in that directory are text, because a JSON number above 2^53
 * does not survive a JavaScript parser and C parsers disagree about integer versus double. So
 * the conversion is here rather than in the document.
 *
 * @throws std::runtime_error where the member is missing or is not entirely a number
 */
inline int64_t requiredDecimal(arnm_json_value *object, const char *key)
{
    const std::string text = requiredString(object, key);
    std::size_t consumed = 0;
    const long long value = std::stoll(text, &consumed);

    if (consumed != text.size())
        throw std::runtime_error(std::string("member \"") + key + "\" is not a decimal number: " +
                                 text);
    return static_cast<int64_t>(value);
}

/**
 * @return the member as a handle, or nullptr where it is absent
 *
 * @warning A member written as `null` comes back as a handle too. A VALUE entry hands a value
 *          over untouched, and arnm 0.7.5 has no call that says what one is -- so `null` and an
 *          object are the same answer here. Where a field is `object-or-null`, use
 *          @ref objectWith() and name a member the object always carries.
 */
inline arnm_json_value *nullableObject(arnm_json_value *object, const char *key)
{
    arnm_json_value *value = nullptr;
    arnm_json_field field {key, static_cast<uint32_t>(std::strlen(key)),
                           ARNM_JSON_FIELD_TYPE_VALUE, &value};
    uint64_t found = 0;

    arnm_json_read_object(object, &field, 1, &found);
    return found == 0 ? nullptr : value;
}

/**
 * @p key as an object, recognised by a member it always carries.
 *
 * The way to read an `object-or-null` field: reading @p witness out of a `null` is refused and
 * reading it out of the object is not, which is the discrimination arnm itself does not offer.
 *
 * @param witness a member the object always has; its type does not matter, only that it is there
 * @return the member as a handle, or nullptr where it is absent, `null`, or carries no @p witness
 */
inline arnm_json_value *objectWith(arnm_json_value *object, const char *key, const char *witness)
{
    arnm_json_value *value = nullableObject(object, key);
    if (value == nullptr)
        return nullptr;

    arnm_json_value *probe = nullptr;
    arnm_json_field field {witness, static_cast<uint32_t>(std::strlen(witness)),
                           ARNM_JSON_FIELD_TYPE_VALUE, &probe};
    uint64_t found = 0;

    arnm_json_read_object(value, &field, 1, &found);
    return found == 0 ? nullptr : value;
}

/** @throws std::runtime_error where the member is absent or `null` */
inline arnm_json_value *requiredObject(arnm_json_value *object, const char *key)
{
    arnm_json_value *value = nullableObject(object, key);
    if (value == nullptr)
        throw std::runtime_error(std::string("required member \"") + key + "\" is missing");
    return value;
}

inline void VectorFile::read_envelope(const std::string &path, arnm_json_value *root)
{
    root_ = root;

    if (requiredNumber(root, "contractVersion") != 1)
        throw std::runtime_error(path + ": contractVersion is not 1");
    if (requiredString(root, "kind") != "test-vectors")
        throw std::runtime_error(path + ": kind is not test-vectors");
    if (requiredString(root, "subject") != subject_)
        throw std::runtime_error(path + ": subject is not " + subject_);

    const int64_t declared = requiredNumber(root, "count");
    arnm_json_value *array = requiredObject(root, "vectors");

    /* All or nothing: arnm refuses an array longer than the buffer rather than truncating it, so
     * asking with room for the declared count and then comparing is enough to catch a file that
     * carries more vectors than it admits to. */
    if (declared <= 0)
        throw std::runtime_error(path + ": a subject with no vectors passes by saying nothing");
    vectors_.assign(static_cast<size_t>(declared), nullptr);
    uint32_t actual = 0;
    if (arnm_json_read_array(array, vectors_.data(), static_cast<uint32_t>(declared), &actual) !=
        ARNM_SUCCESS)
        throw std::runtime_error(path + ": vectors is no array, or is longer than the declared " +
                                 std::to_string(declared));
    if (actual != static_cast<uint32_t>(declared))
        throw std::runtime_error(path + ": declares count " + std::to_string(declared) +
                                 " and carries " + std::to_string(actual) + " vectors");

    std::set<std::string> ids;
    for (arnm_json_value *vector : vectors_) {
        const std::string id = requiredString(vector, "id");
        if (id.rfind(subject_ + ".", 0) != 0)
            throw std::runtime_error(path + ": id \"" + id + "\" does not name its subject");
        if (!ids.insert(id).second)
            throw std::runtime_error(path + ": id \"" + id +
                                     "\" appears twice; an id names one vector forever");
    }
}

/**
 * The decimal-string form of a `contracts/` typed value -- `{ "type": ..., "value": "..." }`.
 *
 * @throws std::runtime_error where the member is missing or carries no `value`
 */
inline std::string contractValue(arnm_json_value *object, const char *key)
{
    return requiredString(requiredObject(object, key), "value");
}

} // namespace contract

#endif /* FS_TESTS_CONTRACT_VECTORS_HPP */
