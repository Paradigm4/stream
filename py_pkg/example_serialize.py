"""Usage:

> python example_serialize.py
[(0, 0, 0, (255,  1.))]
"""

import dill
import numpy
import scidbpy
import scidbstrm


db = scidbpy.connect()


def get_first(df):
    return df.head(1)


# Serialize (pack) and Upload function to SciDB
ar_fun = db.input(upload_data=scidbstrm.pack_func(get_first)).store()


que = db.stream(
    'build(<x:double>[i=1:5], i)',
    """'python -uc "
import scidbstrm

map_fun = scidbstrm.read_func()
scidbstrm.map(map_fun)

"'""",
    "'format=feather'",
    "'types=double'",
    '_sg({}, 0)'.format(ar_fun.name)  # Array with Serialized function
)

print(que[:])
