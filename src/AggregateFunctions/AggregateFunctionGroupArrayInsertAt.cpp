#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <AggregateFunctions/FactoryHelpers.h>

#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>

#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypesNumber.h>

#include <Columns/ColumnArray.h>

#include <Common/FieldVisitorToString.h>
#include <Common/FieldVisitorConvertToNumber.h>
#include <Common/assert_cast.h>
#include <Interpreters/convertFieldToType.h>

#include <AggregateFunctions/IAggregateFunction.h>

constexpr size_t AGGREGATE_FUNCTION_GROUP_ARRAY_INSERT_AT_MAX_SIZE = 0xFFFFFF;


namespace DB
{

struct Settings;

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int TOO_MANY_ARGUMENTS_FOR_FUNCTION;
    extern const int TOO_LARGE_ARRAY_SIZE;
    extern const int CANNOT_CONVERT_TYPE;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
}

namespace
{

/** Aggregate function, that takes two arguments: value and position,
  *  and as a result, builds an array with values are located at corresponding positions.
  *
  * If more than one value was inserted to single position, the any value (first in case of single thread) is stored.
  * If no values was inserted to some position, then default value will be substituted.
  *
  * Aggregate function also accept optional parameters:
  * - default value to substitute;
  * - length to resize result arrays (if you want to have results of same length for all aggregation keys);
  *
  * If you want to pass length, default value should be also given.
  */


/// Generic case (inefficient).
struct AggregateFunctionGroupArrayInsertAtDataGeneric
{
    Array value;    /// TODO Add MemoryTracker
};


class AggregateFunctionGroupArrayInsertAtGeneric final
    : public IAggregateFunctionDataHelper<AggregateFunctionGroupArrayInsertAtDataGeneric, AggregateFunctionGroupArrayInsertAtGeneric>
{
private:
    DataTypePtr type;
    SerializationPtr serialization;
    Field default_value;
    UInt64 length_to_resize = 0;    /// zero means - do not do resizing.

public:
    AggregateFunctionGroupArrayInsertAtGeneric(const DataTypes & arguments, const Array & params)
        : IAggregateFunctionDataHelper<AggregateFunctionGroupArrayInsertAtDataGeneric, AggregateFunctionGroupArrayInsertAtGeneric>(arguments, params, std::make_shared<DataTypeArray>(arguments[0]))
        , type(argument_types[0])
        , serialization(type->getDefaultSerialization())
    {
        if (!params.empty())
        {
            if (params.size() > 2)
                throw Exception(ErrorCodes::TOO_MANY_ARGUMENTS_FOR_FUNCTION, "Aggregate function {} requires at most two parameters.", getName());

            default_value = params[0];

            if (params.size() == 2)
            {
                length_to_resize = applyVisitor(FieldVisitorConvertToNumber<UInt64>(), params[1]);
                if (length_to_resize > AGGREGATE_FUNCTION_GROUP_ARRAY_INSERT_AT_MAX_SIZE)
                    throw Exception(ErrorCodes::TOO_LARGE_ARRAY_SIZE,
                                    "Too large array size (maximum: {})", AGGREGATE_FUNCTION_GROUP_ARRAY_INSERT_AT_MAX_SIZE);
            }
        }

        if (!isUInt(arguments[1]))
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "Second argument of aggregate function {} must be unsigned integer.", getName());

        if (default_value.isNull())
            default_value = type->getDefault();
        else
        {
            Field converted = convertFieldToType(default_value, *type);
            if (converted.isNull())
                throw Exception(ErrorCodes::CANNOT_CONVERT_TYPE, "Cannot convert parameter of aggregate function {} ({}) "
                                "to type {} to be used as default value in array",
                                getName(), applyVisitor(FieldVisitorToString(), default_value), type->getName());

            default_value = converted;
        }
    }

    String getName() const override { return "groupArrayInsertAt"; }

    bool allocatesMemoryInArena() const override { return false; }

    void add(AggregateDataPtr __restrict place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        /// TODO Do positions need to be 1-based for this function?
        size_t position = columns[1]->getUInt(row_num);

        /// If position is larger than size to which array will be cut - simply ignore value.
        if (length_to_resize && position >= length_to_resize)
            return;

        if (position >= AGGREGATE_FUNCTION_GROUP_ARRAY_INSERT_AT_MAX_SIZE)
            throw Exception(ErrorCodes::TOO_LARGE_ARRAY_SIZE, "Too large array size: "
                "position argument ({}) is greater or equals to limit ({})",
                position, AGGREGATE_FUNCTION_GROUP_ARRAY_INSERT_AT_MAX_SIZE);

        Array & arr = data(place).value;

        if (arr.size() <= position)
            arr.resize(position + 1);
        else if (!arr[position].isNull())
            return; /// Element was already inserted to the specified position.

        columns[0]->get(row_num, arr[position]);
    }

    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        Array & arr_lhs = data(place).value;
        const Array & arr_rhs = data(rhs).value;

        if (arr_lhs.size() < arr_rhs.size())
            arr_lhs.resize(arr_rhs.size());

        for (size_t i = 0, size = arr_rhs.size(); i < size; ++i)
            if (arr_lhs[i].isNull() && !arr_rhs[i].isNull())
                arr_lhs[i] = arr_rhs[i];
    }

    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf, std::optional<size_t> /* version */) const override
    {
        const Array & arr = data(place).value;
        size_t size = arr.size();
        writeVarUInt(size, buf);

        for (const Field & elem : arr)
        {
            if (elem.isNull())
            {
                writeBinary(UInt8(1), buf);
            }
            else
            {
                writeBinary(UInt8(0), buf);
                serialization->serializeBinary(elem, buf, {});
            }
        }
    }

    void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf, std::optional<size_t> /* version */, Arena *) const override
    {
        size_t size = 0;
        readVarUInt(size, buf);

        if (size > AGGREGATE_FUNCTION_GROUP_ARRAY_INSERT_AT_MAX_SIZE)
            throw Exception(ErrorCodes::TOO_LARGE_ARRAY_SIZE,
                            "Too large array size (maximum: {})", AGGREGATE_FUNCTION_GROUP_ARRAY_INSERT_AT_MAX_SIZE);

        Array & arr = data(place).value;

        arr.resize(size);
        for (size_t i = 0; i < size; ++i)
        {
            UInt8 is_null = 0;
            readBinary(is_null, buf);
            if (!is_null)
                serialization->deserializeBinary(arr[i], buf, {});
        }
    }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        ColumnArray & to_array = assert_cast<ColumnArray &>(to);
        IColumn & to_data = to_array.getData();
        ColumnArray::Offsets & to_offsets = to_array.getOffsets();

        const Array & arr = data(place).value;

        for (const Field & elem : arr)
        {
            if (!elem.isNull())
                to_data.insert(elem);
            else
                to_data.insert(default_value);
        }

        size_t result_array_size = length_to_resize ? length_to_resize : arr.size();

        /// Pad array if need.
        for (size_t i = arr.size(); i < result_array_size; ++i)
            to_data.insert(default_value);

        to_offsets.push_back(to_offsets.back() + result_array_size);
    }
};


AggregateFunctionPtr createAggregateFunctionGroupArrayInsertAt(
    const std::string & name, const DataTypes & argument_types, const Array & parameters, const Settings *)
{
    assertBinary(name, argument_types);

    if (argument_types.size() != 2)
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Aggregate function groupArrayInsertAt requires two arguments.");

    return std::make_shared<AggregateFunctionGroupArrayInsertAtGeneric>(argument_types, parameters);
}

}

void registerAggregateFunctionGroupArrayInsertAt(AggregateFunctionFactory & factory)
{
    factory.registerFunction("groupArrayInsertAt", createAggregateFunctionGroupArrayInsertAt);
}

}
