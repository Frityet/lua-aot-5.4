local export = {}

function export.add(x, y)
    return y + x
end

function export.sub(x, y)
    return x - y
end

function export.mul(x, y)
    return x * y
end

function export.div(x, y)
    return x / y
end

return export
